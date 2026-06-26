// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/semantic_analyzer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

#include "compiler/error_collector.h"
#include "compiler/logging.h"
#include "compiler/type_context.h"
#include "compiler/type_resolver.h"

namespace {

class AutoScope {
 public:
  AutoScope(ScopeManager& ctx,
            ScopeManager::ScopeType type,
            std::string_view name)
      : ctx_(ctx) {
    ctx_.EnterScope(type, name);
  }
  ~AutoScope() { ctx_.ExitScope(); }

 private:
  ScopeManager& ctx_;
};

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

using LiteralType = TypeRegistry::LiteralType;

}  // namespace

SemanticAnalyzer::SemanticAnalyzer(TypeContext& type_context,
                                   ScopeManager& scope_manager,
                                   ErrorCollector& error_collector,
                                   TypeRegistry& type_registry)
    : type_context_(type_context),
      scope_manager_(scope_manager),
      error_collector_(error_collector),
      type_registry_(type_registry) {}

void SemanticAnalyzer::Check(Block& block, FunctionContext& context) {
  // Collect user-defined type names to ensure they're available for function
  // signatures, field types, etc. This also allows for recursive types (e.g. a
  // struct that has a field of its own type).
  std::vector<std::pair<NamedBinding, StructDeclaration*>> struct_decls;
  for (auto& statement : block.statements) {
    if (auto* declaration = std::get_if<StructDeclaration>(&statement->as)) {
      struct_decls.emplace_back(type_registry_.NewStructSymbol(*declaration),
                                declaration);
    }
  }

  // Collect alias to types. This needs to happen after the initial struct names
  // are populated in the scope but before the types are actually instantiated.
  for (auto& statement : block.statements) {
    if (auto* alias = std::get_if<TypeAliasStatement>(&statement->as)) {
      // If this is an alias to a specific type, it acts more like a link with
      // no new type created just another binding to the existing type under a
      // new name. This ensures aliased types can still be constructed, etc.
      if (auto* target = std::get_if<std::string>(&alias->type->type)) {
        std::optional<NamedBinding> target_binding =
            scope_manager_.FindBindingFor(*target, ScopeManager::All);
        if (!target_binding->IsType()) {
          error_collector_.Add("Cannot create type alias '" +
                                   alias->name.value +
                                   "' from value identifier '" + *target + "'",
                               alias->type->metadata);
        }
        scope_manager_.InsertNameIntoScope(
            alias->name.value, NamedBinding::TypeAlias,
            target_binding->realized_type_id, target_binding->symbol_id,
            target_binding->idx, target_binding->parent_type_id);
      } else {
        // In the case of an alias to a more complex type we do assign a new
        // type to allow for recursive definitions i.e. alias Foo = Array[Foo].
        type_context_.GetAliasOf(alias->name.value, *alias->type);
      }
    }
  }

  // Type check the full struct bodies (and functions) once all types are known.
  // Errors are logged from within `DefineStructType` and `DefineFunction`.
  for (auto& [self, declaration] : struct_decls) {
    if (!self.realized_type_id)  // Only handle non-template (concrete) types
      continue;

    CHECK(self.symbol_id.has_value());  // DeclareStructSymbol MUST provide one
    auto* symbol = type_registry_.GetSymbol<StructSymbol>(*self.symbol_id);
    CHECK(symbol);  // DeclareStructSymbol MUST create a StructSymbol

    type_context_.DefineStructType(*self.realized_type_id, *symbol,
                                   /*template_arguments=*/{});
  }
  for (auto& statement : block.statements) {
    if (auto* declaration = std::get_if<FunctionDeclaration>(&statement->as)) {
      SymbolId symbol_id = type_registry_.NewFunctionSymbol(*declaration);
      type_context_.DefineFunction(symbol_id);
    }
  }

  for (auto& statement : block.statements) {
    CheckStatement(statement, context);
  }

  std::vector<TypeContext::RealizedFunction> function_bodies =
      type_context_.GetRealizedFunctions();

  while (true) {
    if (function_bodies.empty())
      break;

    for (auto realized_function : function_bodies) {
      if (!realized_function.delcaration.body)
        continue;

      scope_manager_.WithScope(realized_function.scope_id, [&]() {
        FunctionContext fn_context{{}, realized_function.return_type_id};
        Check(*realized_function.delcaration.body, fn_context);
        realized_function.delcaration.resolved->required_captures =
            std::move(fn_context.required_captures);
      });
    }

    function_bodies = type_context_.GetRealizedFunctions();
  }
}

void SemanticAnalyzer::CheckStatement(std::unique_ptr<Statement>& statement,
                                      FunctionContext& context) {
  std::visit(
      Overloaded{
          [&](std::unique_ptr<Expression>& expr) {
            CheckExpression(expr, context);
          },
          [&](FunctionDeclaration& fn) {
            // Function bodies are checked only when a FunctionType is realized
          },
          [&](ReturnStatement& ret) {
            Result return_result = CheckExpression(ret.value, context);

            if (!return_result.has_value())
              return;

            if (return_result->binding && return_result->binding->IsType()) {
              NamedBinding binding = return_result->binding.value();
              error_collector_.Add(
                  "Expected a variable or value, but found type '" +
                      binding.name + "'",
                  ret.value->meta);
              return;
            }

            if (!type_context_.IsTypeSubsetOf(*return_result->type_id,
                                              context.return_type_id)) {
              error_collector_.Add(
                  "Returning " +
                      type_registry_.GetNameFromTypeId(
                          *return_result->type_id) +
                      " from function with return type " +
                      type_registry_.GetNameFromTypeId(context.return_type_id),
                  statement->meta);
            }
          },
          [&](ThrowStatement& thr) { CheckExpression(thr.value, context); },
          [&](IfStatement& if_stmt) {
            Result result = CheckExpression(if_stmt.condition, context);

            const auto narrowing_info = result
                                            ? result->narrowing_info
                                            : std::vector<ScopeNarrowingInfo>{};

            {
              AutoScope _{scope_manager_, ScopeManager::BlockScope, "if"};
              for (const auto& narrowing : narrowing_info) {
                scope_manager_.DeclareNarrowedBinding(narrowing.symbol,
                                                      narrowing.if_branch_type);
              }

              Check(if_stmt.then_body, context);
            }
            {
              AutoScope _{scope_manager_, ScopeManager::BlockScope, "else"};
              for (const auto& narrowing : narrowing_info) {
                scope_manager_.DeclareNarrowedBinding(
                    narrowing.symbol, narrowing.else_branch_type);
              }

              Check(if_stmt.else_body, context);
            }
          },
          [&](WhileStatement& while_stmt) {
            CheckExpression(while_stmt.condition, context);
            {
              AutoScope _{scope_manager_, ScopeManager::BlockScope, "while"};
              Check(while_stmt.body, context);
            }
          },
          // `break` and `continue` are single word statements.
          [&](const BreakStatement&) {},
          [&](const ContinueStatement&) {},
          [&](AssignStatement& assign) {
            // Check this is the first assignment in this scope with that name.
            if (scope_manager_.FindBindingFor(assign.name,
                                              ScopeManager::Current)) {
              LOG(WARNING) << "Identifier already declared with name: '"
                           << assign.name << "'";
              return;
            }

            // Ensure assignment expression's type matches the declared type (if
            // given, otherwise the variable's type is deduced from the value).
            std::optional<TypeId> parsed_type_id;
            if (assign.type.has_value()) {
              parsed_type_id = type_context_.GetTypeIdFor(*assign.type);
            }

            Result value_result = CheckExpression(assign.value, context);
            if (!value_result.has_value())
              return;

            if (!value_result->has_type_id()) {
              error_collector_.Add(
                  "Unable to assign from invalid RHS expression",
                  assign.value->meta);
              return;
            }

            if (parsed_type_id.has_value()) {
              if (!type_context_.IsTypeSubsetOf(*value_result->type_id,
                                                *parsed_type_id)) {
                error_collector_.Add(
                    "Assigning " +
                        type_registry_.GetNameFromTypeId(
                            *value_result->type_id) +
                        " to " +
                        type_registry_.GetNameFromTypeId(*parsed_type_id),
                    statement->meta);
                return;
              }
            } else {
              parsed_type_id = value_result->type_id;
            }

            // Register the variable's type within the current scope.
            NamedBinding symbol = scope_manager_.DeclareVariableBinding(
                assign.name, parsed_type_id.value());
            assign.resolved = ResolvedIdentifier{symbol};
          },
          [&](StructDeclaration& struct_decl) {
            // Function bodies are checked only when a FunctionType is realized
          },
          [&](const ImportStatement& import) { /*nothing to check*/ },
          [&](const TypeAliasStatement& alias) { /* nothing to check */ },
      },
      statement->as);
}

SemanticAnalyzer::Result SemanticAnalyzer::CheckExpression(
    std::unique_ptr<Expression>& expression,
    FunctionContext& context) {
  Result result = std::visit(
      Overloaded{
          [&](PrimaryExpression& primary) -> SemanticAnalyzer::Result {
            return std::visit(
                Overloaded{
                    [&](const StringLiteral&) -> SemanticAnalyzer::Result {
                      if (auto binding = scope_manager_.FindBindingFor(
                              "String", ScopeManager::All)) {
                        return ExpressionResult(*binding->realized_type_id);
                      }

                      error_collector_.Add("unknown identifier: String",
                                           expression->meta);
                      return std::nullopt;
                    },
                    [&](Identifier& ident) -> SemanticAnalyzer::Result {
                      if (ident.name == "Nil")
                        return ExpressionResult(TypeRegistry::Nil);

                      // Search within the current function scope for value.
                      auto binding = scope_manager_.FindBindingFor(
                          ident.name, ScopeManager::Function);
                      if (binding) {
                        ident.resolved = ResolvedIdentifier{*binding};
                        return ExpressionResult(*binding);
                      }

                      // Fallback search to the parent function scope.
                      binding = scope_manager_.FindBindingFor(
                          ident.name, ScopeManager::Closure);
                      if (binding) {
                        // Any value symbols found now must be captured.
                        if (binding->kind == NamedBinding::Variable ||
                            binding->kind == NamedBinding::Capture) {
                          context.required_captures.push_back(*binding);
                          // Variables will ALWAYS have a realized TypeId.
                          binding = scope_manager_.DeclareCaptureBinding(
                              ident.name, *binding->realized_type_id);
                        }

                        ident.resolved = ResolvedIdentifier{*binding};
                        return ExpressionResult(*binding);
                      }

                      // Fallback seatch to ALL scopes for functions/structs.
                      binding = scope_manager_.FindBindingFor(
                          ident.name, ScopeManager::All);
                      if (binding && (binding->kind == NamedBinding::Function ||
                                      binding->kind == NamedBinding::Struct)) {
                        ident.resolved = ResolvedIdentifier{*binding};
                        return ExpressionResult(*binding);
                      }

                      error_collector_.Add("unknown identifier: " + ident.name,
                                           expression->meta);
                      return std::nullopt;
                    },
                    [&](int32_t) -> SemanticAnalyzer::Result {
                      return ExpressionResult(LiteralType::i32);
                    },
                    [&](const CodepointLiteral& codepoint)
                        -> SemanticAnalyzer::Result {
                      return ExpressionResult(LiteralType::Codepoint);
                    },
                    [&](float) -> SemanticAnalyzer::Result {
                      return ExpressionResult(LiteralType::f32);
                    },
                    [&](bool) -> SemanticAnalyzer::Result {
                      return ExpressionResult(LiteralType::Bool);
                    },
                    [&](Nil) -> SemanticAnalyzer::Result {
                      return ExpressionResult(LiteralType::Nil);
                    }},
                primary.value);
          },
          [&](BinaryExpression& binary) -> SemanticAnalyzer::Result {
            Result lhs = CheckExpression(binary.lhs, context);
            Result rhs = CheckExpression(binary.rhs, context);

            if (!lhs.has_value() || !rhs.has_value())
              return std::nullopt;

            if (!lhs->has_type_id() || !rhs->has_type_id()) {
              error_collector_.Add(
                  "Template types cannot be used in binary expressions without "
                  "instantiation",
                  expression->meta);
              return std::nullopt;
            }

            if (lhs->type_id != rhs->type_id &&
                !((rhs->type_id == LiteralType::Nil &&
                   type_context_.IsTypeSubsetOf(*rhs->type_id,
                                                *lhs->type_id)) ||
                  (lhs->type_id == LiteralType::Nil &&
                   type_context_.IsTypeSubsetOf(*lhs->type_id,
                                                *rhs->type_id)))) {
              error_collector_.Add("LHS and RHS are not compatible",
                                   expression->meta);
              error_collector_.Add(
                  "LHS type is " +
                      type_registry_.GetNameFromTypeId(*lhs->type_id),
                  binary.lhs->meta);
              error_collector_.Add(
                  "But RHS type is " +
                      type_registry_.GetNameFromTypeId(*rhs->type_id),
                  binary.rhs->meta);
              return std::nullopt;
            }

            ResolvedBinary resolved{ResolvedBinary::Specialization::Number};
            std::vector<ScopeNarrowingInfo> narrowing_info;
            if (lhs->type_id ==
                type_context_.GetTypeIdFor(ParsedType{"String"})) {
              resolved.specialization = ResolvedBinary::Specialization::String;
            } else if (lhs->type_id == LiteralType::Nil ||
                       rhs->type_id == LiteralType::Nil) {
              resolved.specialization = ResolvedBinary::Specialization::Nil;

              // TODO: Ensure that NamedBinding is referring to a Variable.
              std::optional<NamedBinding> symbol_to_narrow;
              if (lhs->type_id == LiteralType::Nil &&
                  rhs->binding.has_value()) {
                symbol_to_narrow = rhs->binding.value();
              } else if (rhs->type_id == LiteralType::Nil &&
                         lhs->binding.has_value()) {
                symbol_to_narrow = lhs->binding.value();
              }

              if (symbol_to_narrow) {
                auto unwrapped_type_id = type_context_.UnwrapOptionalTypeId(
                    symbol_to_narrow->realized_type_id.value());

                // This should NEVER be hit. See compatibility check above.
                CHECK(unwrapped_type_id)
                    << "Non-optional type cannot be narrowed from Nil";

                TypeId if_branch_type = unwrapped_type_id.value();
                TypeId else_branch_type = LiteralType::Nil;
                if (binary.op != TokenKind::kCompareNe) {
                  std::swap(if_branch_type, else_branch_type);
                }

                narrowing_info.push_back(
                    ScopeNarrowingInfo{symbol_to_narrow.value(), if_branch_type,
                                       else_branch_type});
              }
            }
            binary.resolved = std::move(resolved);

            // Comparison operators will always generate a boolean
            if (binary.op == TokenKind::kCompareGt ||
                binary.op == TokenKind::kCompareLt ||
                binary.op == TokenKind::kCompareGe ||
                binary.op == TokenKind::kCompareLe ||
                binary.op == TokenKind::kCompareEq ||
                binary.op == TokenKind::kCompareNe) {
              ExpressionResult result(LiteralType::Bool);
              result.narrowing_info = std::move(narrowing_info);
              return result;
            }

            return ExpressionResult(*lhs->type_id);
          },
          [&](CallExpression& call_expr) -> SemanticAnalyzer::Result {
            Result callee_result = CheckExpression(call_expr.callee, context);

            // Short-circuit if the callee was invalid.
            if (!callee_result.has_value())
              return std::nullopt;

            Result type_check_result = TypeCheckCallExpr(
                call_expr, callee_result.value(), context, expression->meta);

            return type_check_result;
          },
          [&](AssignmentExpression& assign) -> SemanticAnalyzer::Result {
            Result lhs = CheckExpression(assign.lhs, context);
            Result rhs = CheckExpression(assign.rhs, context);

            if (!lhs.has_value() || !rhs.has_value())
              return std::nullopt;

            if (!rhs->has_type_id()) {
              error_collector_.Add(
                  "Unable to assign from invalid RHS expression",
                  assign.rhs->meta);
              return std::nullopt;
            }

            if (!lhs->binding.has_value() ||
                !(lhs->binding->kind == NamedBinding::Kind::Variable ||
                  lhs->binding->kind == NamedBinding::Kind::Field) ||
                !lhs->has_type_id()) {
              error_collector_.Add("Unable to assign to invalid LHS expression",
                                   assign.rhs->meta);
              return std::nullopt;
            }

            if (!type_context_.IsTypeSubsetOf(*rhs->type_id, *lhs->type_id)) {
              error_collector_.Add(
                  "Mismatched assignment: " +
                      type_registry_.GetNameFromTypeId(*lhs->type_id) +
                      " vs. " + type_registry_.GetNameFromTypeId(*rhs->type_id),
                  expression->meta);
              return std::nullopt;
            }
            return rhs;
          },
          [&](MemberAccessExpression& member_access)
              -> SemanticAnalyzer::Result {
            Result object_result =
                CheckExpression(member_access.object, context);
            if (!object_result)
              return std::nullopt;

            if (!object_result->has_type_id()) {
              error_collector_.Add(
                  "Cannot access members on an uninstantiated template "
                  "blueprint",
                  member_access.object->meta);
              return std::nullopt;
            }

            if (const auto* const struct_type =
                    type_registry_.GetType<StructType>(
                        *object_result->type_id)) {
              const auto& member_name = member_access.member_name;
              if (auto binding = scope_manager_.FindBindingFor(
                      member_name, ScopeManager::Current,
                      struct_type->scope_id)) {
                if (binding->kind == NamedBinding::Field) {
                  CHECK(binding->idx.has_value())
                      << "Member symbol must have an index for member access";
                  member_access.resolved = ResolvedAccess{binding->idx.value()};
                }
                return ExpressionResult(*binding);
              } else {
                error_collector_.Add("No member '" + member_name + "' on " +
                                         struct_type->declaration.name,
                                     expression->meta);
              }
            } else {
              if (auto unwrapped_type_id = type_context_.UnwrapOptionalTypeId(
                      *object_result->type_id)) {
                error_collector_.Add(
                    "Attempting member access on Optional type: " +
                        type_registry_.GetNameFromTypeId(
                            *object_result->type_id),
                    member_access.object->meta);
              } else {
                error_collector_.Add("Type " +
                                         type_registry_.GetNameFromTypeId(
                                             *object_result->type_id) +
                                         " does not support member access",
                                     member_access.object->meta);
              }
            }
            return std::nullopt;
          },
          [&](ArrayAccessExpression& array_access) -> SemanticAnalyzer::Result {
            Result object = CheckExpression(array_access.array, context);
            Result index = CheckExpression(array_access.index, context);

            if (!object.has_value() || !object->has_type_id() ||
                !index.has_value() || !index->has_type_id())
              return std::nullopt;

            if (index->type_id != LiteralType::i32) {
              error_collector_.Add(
                  "Index must be i32 but instead type is: " +
                      type_registry_.GetNameFromTypeId(*index->type_id),
                  array_access.index->meta);
              // Continue parsing to collect more errors.
            }

            // FIXME: Once templates exist we can narrot the type here.
            return ExpressionResult{LiteralType::Any};
          },
          [&](LogicExpression& logic) -> SemanticAnalyzer::Result {
            Result lhs = CheckExpression(logic.lhs, context);
            Result rhs = CheckExpression(logic.rhs, context);

            if (!lhs.has_value() || !lhs->has_type_id() || !rhs.has_value() ||
                !rhs->has_type_id())
              return std::nullopt;

            if (lhs->type_id != rhs->type_id) {
              error_collector_.Add("LHS and RHS are not compatible",
                                   expression->meta);
              error_collector_.Add(
                  "LHS type is " +
                      type_registry_.GetNameFromTypeId(*lhs->type_id),
                  logic.lhs->meta);
              error_collector_.Add(
                  "But RHS type is " +
                      type_registry_.GetNameFromTypeId(*rhs->type_id),
                  logic.rhs->meta);
              return std::nullopt;
            }

            std::vector<ScopeNarrowingInfo> narrowing_info;
            if (logic.kind == LogicExpression::Kind::AND) {
              narrowing_info = lhs->narrowing_info;
              narrowing_info.insert(narrowing_info.end(),
                                    rhs->narrowing_info.begin(),
                                    rhs->narrowing_info.end());
            }

            ExpressionResult result(LiteralType::Bool);
            result.narrowing_info = std::move(narrowing_info);
            return result;
          },
          [&](ClosureExpression& closure) -> SemanticAnalyzer::Result {
            SymbolId symbol_id = type_registry_.NewFunctionSymbol(closure.fn);
            if (auto binding = type_context_.DefineFunction(symbol_id)) {
              return ExpressionResult(*binding);
            }
            CHECK(false) << "Failed to declare symbol for closure";
            return std::nullopt;
          },
          [&](PrefixUnaryExpression& prefix) -> SemanticAnalyzer::Result {
            Result operand = CheckExpression(prefix.operand, context);
            // TODO: Check that `op` is valid for `operand`.
            return operand;
          },
          [&](PostfixUnaryExpression& postfix) -> SemanticAnalyzer::Result {
            Result operand = CheckExpression(postfix.operand, context);
            // TODO: Check that `op` is valid for `operand`.
            return operand;
          },
          [&](TypeCastExpression& cast) -> SemanticAnalyzer::Result {
            Result original_result = CheckExpression(cast.expr, context);
            if (!original_result.has_value())
              return std::nullopt;

            std::optional<TypeId> as_type =
                type_context_.GetTypeIdFor(cast.as_type);
            if (!as_type) {
              error_collector_.Add("Unknown 'as' type", cast.as_type.metadata);
              return std::nullopt;
            }

            if (!original_result->has_type_id()) {
              error_collector_.Add("Can not cast unrealized expression",
                                   cast.expr->meta);
            }

            bool is_valid_cast = type_context_.IsTypeSubsetOf(
                as_type.value(), *original_result->type_id);

            // Allow explicit casts between i32 <=> Codepoint.
            is_valid_cast |=
                (as_type.value() == LiteralType::Codepoint &&
                 original_result->type_id == LiteralType::i32) ||
                (original_result->type_id == LiteralType::Codepoint &&
                 as_type.value() == LiteralType::i32);

            // Allow explicit casts from i32 to f32. This is lossy for large
            // integers but common (most languages implicitly allow this cast).
            // Since Codepoints can be cast to i32 allow to f32 for consistency.
            is_valid_cast |=
                (as_type.value() == LiteralType::f32 &&
                 original_result->type_id == LiteralType::i32) ||
                (as_type.value() == LiteralType::f32 &&
                 original_result->type_id == LiteralType::Codepoint);

            if (!is_valid_cast) {
              error_collector_.Add(
                  "Invalid type cast from " +
                      type_registry_.GetNameFromTypeId(
                          *original_result->type_id) +
                      " to " + type_registry_.GetNameFromTypeId(*as_type),
                  cast.as_type.metadata);
              return std::nullopt;
            }

            TypeId result_type_id = as_type.value();
            if (cast.strategy == TypeCastStrategy::OPTIONAL) {
              result_type_id =
                  type_context_.WrapTypeIdAsOptional(as_type.value());
            }

            return ExpressionResult(result_type_id, original_result->binding);
          },
          [&](OptionalChainExpression& optional_chain) -> Result {
            // OptionalChainExpression is a "pseudo-AST node" which represents
            // the END of a chain of ?. accesses (i.e. where to jump to in case
            // of Nil) and resolves to the final type wrapped as an Optional.
            Result result = CheckExpression(optional_chain.root, context);
            if (!result.has_value())
              return std::nullopt;

            CHECK(result->has_type_id())
                << "Only realized results can participate in an optional chain";

            TypeId result_type_id = *result->type_id;
            if (!type_context_.UnwrapOptionalTypeId(result_type_id)) {
              result_type_id =
                  type_context_.WrapTypeIdAsOptional(result_type_id);
            }

            ExpressionResult new_result(result_type_id, result->binding);
            new_result.narrowing_info = std::move(result->narrowing_info);
            return new_result;
          },
          [&](NilCoalescingExpression& coalescing) -> Result {
            Result lhs = CheckExpression(coalescing.lhs, context);
            Result rhs = CheckExpression(coalescing.rhs, context);

            if (!lhs.has_value() || !rhs.has_value())
              return std::nullopt;

            if (!lhs->has_type_id()) {
              error_collector_.Add("does not resolve to a Type",
                                   coalescing.lhs->meta);
              return std::nullopt;
            }

            if (!rhs->has_type_id()) {
              error_collector_.Add("does not resolve to a Type",
                                   coalescing.lhs->meta);
              return std::nullopt;
            }

            if (type_context_.IsTypeNilable(*rhs->type_id)) {
              error_collector_.Add(
                  "RHS should NEVER be Nilable (i.e. Nil | Optional)",
                  coalescing.rhs->meta);
              // Continue parsing to catch more errors...
            }

            std::optional<TypeId> lhs_type_id =
                type_context_.UnwrapOptionalTypeId(*lhs->type_id);
            if (!lhs_type_id) {
              error_collector_.Add("LHS is not optional; ?? is a no-op",
                                   coalescing.lhs->meta);
              return std::nullopt;
            }

            return ExpressionResult{
                type_context_.GetUnionOf({*lhs_type_id, *rhs->type_id})};
          },
          [&](OptionalAccessExpression& optional_access) -> Result {
            Result result = CheckExpression(optional_access.target, context);

            if (!result.has_value())
              return std::nullopt;

            if (!result->has_type_id()) {
              error_collector_.Add("does not resolve to a Type",
                                   optional_access.target->meta);
              return std::nullopt;
            }

            if (auto unwrapped =
                    type_context_.UnwrapOptionalTypeId(*result->type_id)) {
              return ExpressionResult{unwrapped.value(), result->binding};
            }

            error_collector_.Add(
                "Unable to unwrap non-optional type: " +
                    type_registry_.GetNameFromTypeId(*result->type_id),
                optional_access.target->meta);
            return std::nullopt;
          },
          [&](TemplateInstantiationExpression& template_expr) -> Result {
            Result result =
                CheckExpression(template_expr.generic_target, context);

            if (!result.has_value())
              return std::nullopt;

            if (!result->binding.has_value() ||
                (result->binding->kind != NamedBinding::Struct)) {
              error_collector_.Add(".of() used on non-templated type",
                                   template_expr.generic_target->meta);
              return std::nullopt;
            }

            std::vector<TypeId> type_ids;
            type_ids.reserve(template_expr.template_types.size());

            bool encountered_type_error = false;
            for (const auto& type : template_expr.template_types) {
              if (auto type_id = type_context_.GetTypeIdFor(type)) {
                type_ids.push_back(type_id.value());
              } else {
                // Keep parsing the rest of the types even if an error is
                // encountered with one to give as many errors as possible.
                std::stringstream ss;
                ss << "unknown type used as template argument: " << type;
                error_collector_.Add(ss.str(), type.metadata);
                encountered_type_error = true;
              }
            }
            if (encountered_type_error)
              return std::nullopt;

            if (auto type_id =
                    type_context_.GetTemplateOf(*result->binding, type_ids)) {
              return *type_id;
            }

            return std::nullopt;
          },
      },
      expression->as);

  expression->type =
      (result && result->has_type_id()) ? *result->type_id : LiteralType::Void;
  return result;
}

void SemanticAnalyzer::TypeCheckCallArguments(
    const std::vector<ArgumentResult>& call_arugment_results,
    const std::vector<TypeId>& expected_argument_types,
    const Metadata& debug_metadata,
    bool is_variadic_function) {
  size_t supplied_argc = call_arugment_results.size();
  size_t expected_argc = expected_argument_types.size();

  if ((is_variadic_function && supplied_argc < expected_argc) ||
      (!is_variadic_function && supplied_argc != expected_argc)) {
    error_collector_.Add("Wrong number of arguments expected " +
                             std::to_string(expected_argc) + " but got " +
                             std::to_string(supplied_argc),
                         debug_metadata);
  } else {
    // If more arguments are supplied than expected, this is a variadic
    // function and any additional args do not need to be checked ("any" type).
    for (size_t i = 0; i < expected_argument_types.size(); ++i) {
      const auto& argument_result = call_arugment_results[i];
      const auto& expected_type = expected_argument_types[i];

      // Even if an argument expression does not parse correctly, continue
      // on to the next to try to collect as many errors as possible.
      if (!argument_result.result.has_value() ||
          !argument_result.result->has_type_id())
        continue;

      if (!type_context_.IsTypeSubsetOf(*argument_result.result->type_id,
                                        expected_type) &&
          argument_result.metadata.has_value()) {
        error_collector_.Add(
            "Argument type mismatch. Expected " +
                type_registry_.GetNameFromTypeId(expected_type) + " but got " +
                type_registry_.GetNameFromTypeId(
                    *argument_result.result->type_id),
            argument_result.metadata.value());
      }
    }
  }
}

std::optional<TypeId> SemanticAnalyzer::InstantiateType(
    const std::vector<std::pair<std::string, ParsedType>>& parsed_types,
    const std::vector<ArgumentResult>& argument_results,
    const std::vector<TemplateArgument>& template_arguments,
    const std::vector<TemplateArgument>& self_template_arguments,
    NamedBinding binding,
    std::string_view symbol_name,
    std::unordered_map<std::string, TypeId> default_template_type_ids) {
  TypeResolver::Bindings bindings;
  TypeResolver resolver(type_context_, error_collector_);

  std::vector<std::string> template_names;
  {
    template_names.reserve(template_arguments.size());
    std::transform(template_arguments.begin(), template_arguments.end(),
                   std::back_inserter(template_names),
                   [](auto& arg) { return arg.name; });
  }

  for (size_t i = 0; i < parsed_types.size(); ++i) {
    const auto& [name, pattern_type] = parsed_types[i];

    if (i >= argument_results.size() ||
        !argument_results[i].result.has_value()) {
      error_collector_.Add("Missing argument", pattern_type.metadata);
      return std::nullopt;
    }

    if (!argument_results[i].metadata)
      continue;

    auto type_id = argument_results[i].result->type_id;
    if (!type_id) {
      error_collector_.Add("Encountered unrealized argument",
                           *argument_results[i].metadata);
      return std::nullopt;
    }

    auto concrete_type = type_context_.GetParsedTypeFromId(*type_id);

    if (!resolver.Resolve(pattern_type, concrete_type, template_names,
                          bindings)) {
      LOG(WARNING) << "Failed to resolve bindings for: " << symbol_name
                   << " concrete: " << concrete_type
                   << " pattern: " << pattern_type;
    }
  }

  std::vector<TypeId> argument_type_ids;
  argument_type_ids.reserve(self_template_arguments.size());
  for (size_t i = 0; i < self_template_arguments.size(); ++i) {
    if (bindings.contains(self_template_arguments[i].name)) {
      const auto& inferred_type = bindings[self_template_arguments[i].name];
      if (auto type_id = type_context_.GetTypeIdFor(inferred_type)) {
        argument_type_ids.push_back(type_id.value());
      } else {
        LOG(ERROR) << "Template argument has unknown TypeId: "
                   << self_template_arguments[i].name;
        return std::nullopt;
      }
    } else if (default_template_type_ids.contains(template_names[i])) {
      argument_type_ids.push_back(default_template_type_ids[template_names[i]]);
    } else {
      LOG(ERROR) << "Failed to infer template argument with no default: "
                 << self_template_arguments[i].name;
      return std::nullopt;
    }
  }

  return type_context_.GetTemplateOf(binding, argument_type_ids);
}

SemanticAnalyzer::Result SemanticAnalyzer::TypeCheckCallExpr(
    CallExpression& call_expr,
    ExpressionResult callee_result,
    FunctionContext& context,
    Metadata debug_metadata) {
  // Ensure all arguments are type-checked regardless of the target.
  std::vector<ArgumentResult> argument_results;
  argument_results.reserve(call_expr.arguments.size());
  std::transform(call_expr.arguments.begin(), call_expr.arguments.end(),
                 std::back_inserter(argument_results),
                 [this, &context](std::unique_ptr<Expression>& expr) {
                   return ArgumentResult{CheckExpression(expr, context),
                                         expr->meta};
                 });

  std::optional<TypeId> callable_type_id = callee_result.type_id;

  if (!callable_type_id) {
    CHECK(callee_result.binding && callee_result.binding->symbol_id)
        << "SymbolId is required for templates";

    if (const auto* symbol = type_registry_.GetSymbol<FunctionSymbol>(
            *callee_result.binding->symbol_id)) {
      std::vector<ArgumentResult> arg_results = argument_results;
      // Account for the implicit "self" argument for method calls
      if (symbol->declaration.function_kind == FunctionKind::Method) {
        arg_results.insert(arg_results.begin(), ArgumentResult{callee_result});
      }

      std::vector<TemplateArgument> template_arguments =
          symbol->declaration.template_arguments;

      // If this function has a parent i.e. is a method, then we must take in to
      // account the parent's template arguments as well for template resolution
      if (callee_result.binding->parent_type_id) {
        const auto* parent_type = type_registry_.GetType<StructType>(
            *callee_result.binding->parent_type_id);
        CHECK(parent_type) << "TypeId("
                           << *callee_result.binding->parent_type_id
                           << ") does not resolve to a StructType";
        template_arguments.insert(
            template_arguments.end(),
            parent_type->declaration.template_arguments.begin(),
            parent_type->declaration.template_arguments.end());
      }

      callable_type_id = InstantiateType(
          symbol->declaration.arguments, arg_results,
          std::move(template_arguments), symbol->declaration.template_arguments,
          *callee_result.binding, "fn " + symbol->declaration.name,
          symbol->default_template_type_ids);
    }

    else if (const auto* symbol = type_registry_.GetSymbol<StructSymbol>(
                 *callee_result.binding->symbol_id)) {
      callable_type_id = InstantiateType(
          symbol->declaration.fields, argument_results,
          symbol->declaration.template_arguments,
          symbol->declaration.template_arguments, *callee_result.binding,
          "struct " + symbol->declaration.name,
          /*default_template_type_ids=*/{});
    }

    else {
      LOG(ERROR) << "non-template symbol attempting to be instantiated";
      return std::nullopt;
    }
  }

  if (!callable_type_id) {
    return std::nullopt;
  }

  if (const auto* const fn_type =
          type_registry_.GetType<FunctionType>(*callable_type_id)) {
    if (callee_result.binding.has_value() &&
        callee_result.binding->kind == NamedBinding::Function) {
      const FunctionSymbol& symbol = *type_registry_.GetSymbol<FunctionSymbol>(
          *callee_result.binding->symbol_id);

      // Account for the implicit "self" argument for method calls
      if (symbol.declaration.function_kind == FunctionKind::Method) {
        argument_results.insert(argument_results.begin(),
                                ArgumentResult{callee_result});
      }
      call_expr.resolved = ResolvedCall{*callee_result.binding->symbol_id,
                                        symbol.declaration.function_kind};
    } else {
      call_expr.resolved = ResolvedCall{*callee_result.binding->symbol_id,
                                        FunctionKind::Anonymous};
    }

    TypeCheckCallArguments(argument_results, fn_type->arg_types, debug_metadata,
                           fn_type->is_variadic);

    // Even if the arguments can not be properly type checked we should
    // resolve to the return type to prevent cascading errors :^).
    return ExpressionResult{fn_type->return_type};
  }

  if (const auto* const struct_type =
          type_registry_.GetType<StructType>(*callable_type_id)) {
    if (struct_type->declaration.is_extern) {
      error_collector_.Add("extern structs have no constructor",
                           debug_metadata);
      return std::nullopt;
    }

    TypeCheckCallArguments(argument_results, struct_type->field_types,
                           debug_metadata);
    call_expr.resolved = ResolvedCall{0, FunctionKind::Constructor};
    return ExpressionResult{*callable_type_id};
  }

  if (const auto* const alias_type =
          type_registry_.GetType<AliasType>(*callable_type_id)) {
    return TypeCheckCallExpr(call_expr,
                             ExpressionResult{alias_type->target_type_id},
                             context, debug_metadata);
  }

  error_collector_.Add("type is not callable: " +
                           type_registry_.GetNameFromTypeId(*callable_type_id),
                       debug_metadata);
  return std::nullopt;
}