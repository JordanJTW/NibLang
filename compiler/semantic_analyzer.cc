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
        if (!target_binding->IsTypeRef()) {
          error_collector_.Add("Cannot create type alias '" + alias->name.text +
                                   "' from value identifier '" + *target + "'",
                               alias->type->metadata);
        }
        scope_manager_.InsertNameIntoScope(
            alias->name, NamedBinding::TypeAlias,
            target_binding->realized_type_id, target_binding->symbol_id,
            target_binding->idx, target_binding->parent_type_id);
      } else {
        // In the case of an alias to a more complex type we do assign a new
        // type to allow for recursive definitions i.e. alias Foo = Array[Foo].
        type_context_.GetAliasOf(alias->name, *alias->type);
      }
    }
  }

  // Type check the full struct bodies (and functions) once all types are known.
  // Errors are logged from within `DefineStructType` and `DefineFunction`.
  for (auto& [self, declaration] : struct_decls) {
    CHECK(self.symbol_id.has_value());  // DeclareStructSymbol MUST provide one
    auto* symbol = type_registry_.GetSymbol<StructSymbol>(*self.symbol_id);
    CHECK(symbol);  // DeclareStructSymbol MUST create a StructSymbol

    scope_manager_.WithScope(symbol->self_scope_id, [&]() {
      // Create canonical symbols for all instance methods and define static
      // methods with-in the Symbol scope. This ensures there is a single symbol
      // for each method (shared by all instances) and that static methods are
      // able to be found on the struct Symbol directly. :^)
      for (auto& [name, fn] : declaration->methods) {
        SymbolId method_id = type_registry_.NewFunctionSymbol(fn, declaration);
        if (fn.function_kind == FunctionKind::Method) {
          scope_manager_.InsertNameIntoScope(fn.name, NamedBinding::Method,
                                             /*type_id=*/std::nullopt,
                                             method_id);
        } else {
          // Intentionally not passing `self_id` for `static` methods.
          type_context_.DefineFunction(method_id,
                                       TypeContext::CheckFunctionBody::YES);
        }
      }

      if (self.realized_type_id) {  // Only handle non-template (concrete) types
        // DefineStructType() depends on method symbols already being populated.
        type_context_.DefineStructType(*self.realized_type_id, *symbol,
                                       /*template_arguments=*/{});
      }
    });
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
            RequireConcreteValue(expr, context);
          },
          [&](FunctionDeclaration& fn) {
            // Function bodies are checked only when a FunctionType is realized
          },
          [&](ReturnStatement& ret) {
            if (ret.value) {
              Result result = RequireConcreteValue(ret.value, context);

              if (!result.has_value())
                return;

              if (!type_context_.IsTypeSubsetOf(*result->type_id,
                                                context.return_type_id)) {
                error_collector_.Add(
                    "Returning " +
                        type_registry_.GetNameFromTypeId(*result->type_id) +
                        " from function with return type " +
                        type_registry_.GetNameFromTypeId(
                            context.return_type_id),
                    statement->meta);
              }
            } else {
              if (!type_context_.IsTypeSubsetOf(TypeRegistry::Unit,
                                                context.return_type_id)) {
                error_collector_.Add(
                    "Returning `Unit` from function with return type " +
                        type_registry_.GetNameFromTypeId(
                            context.return_type_id),
                    statement->meta);
              }
            }
          },
          [&](ThrowStatement& thr) {
            RequireConcreteValue(thr.value, context);
          },
          [&](IfStatement& if_stmt) {
            Result result = RequireConcreteValue(if_stmt.condition, context);

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
            RequireConcreteValue(while_stmt.condition, context);
            {
              AutoScope _{scope_manager_, ScopeManager::BlockScope, "while"};
              Check(while_stmt.body, context);
            }
          },
          // `break` and `continue` are single word statements.
          [&](const BreakStatement&) {}, [&](const ContinueStatement&) {},
          [&](AssignStatement& assign) {
            // Ensure assignment expression's type matches the declared type (if
            // given, otherwise the variable's type is deduced from the value).
            std::optional<TypeId> parsed_type_id;
            if (assign.type.has_value()) {
              parsed_type_id = type_context_.GetTypeIdFor(*assign.type);
            }

            Result result = RequireConcreteValue(assign.value, context);
            if (!result.has_value())
              return;

            if (parsed_type_id.has_value()) {
              if (!type_context_.IsTypeSubsetOf(*result->type_id,
                                                *parsed_type_id)) {
                std::string expected_type =
                    type_registry_.GetNameFromTypeId(*parsed_type_id);
                error_collector_
                    .Add(
                        "unable to assign `" +
                            type_registry_.GetNameFromTypeId(*result->type_id) +
                            "` to `" + expected_type + "`",
                        assign.value->meta)
                    .WithNote("declared `" + expected_type + "` here",
                              assign.type->metadata);
                return;
              }
            } else {
              parsed_type_id = result->type_id;
            }

            // Register the variable's type within the current scope.
            NamedBinding binding = scope_manager_.DeclareVariableBinding(
                assign.name, parsed_type_id.value());
            assign.resolved = ResolvedIdentifier{binding};
          },
          [&](StructDeclaration& struct_decl) {
            // Function bodies are checked only when a FunctionType is realized
          },
          [&](const ImportStatement& import) { /*nothing to check*/ },
          [&](const TypeAliasStatement& alias) { /* nothing to check */ },
          [&](const InterfaceDeclaration& decl) {
            // Function bodies are checked only when a FunctionType is realized
          }},
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
                        return ExpressionResult::of_binding(*binding);
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
                              binding->name, *binding->realized_type_id);
                        }

                        ident.resolved = ResolvedIdentifier{*binding};
                        return ExpressionResult::of_binding(*binding);
                      }

                      // Fallback search to ALL scopes for top-level
                      // declarations i.e. functions, structs, interfaces,
                      // alias.
                      binding = scope_manager_.FindBindingFor(
                          ident.name, ScopeManager::All);
                      if (binding) {
                        switch (binding->kind) {
                          case NamedBinding::Function:
                          case NamedBinding::Method:
                          case NamedBinding::Struct:
                          case NamedBinding::TypeAlias:
                          case NamedBinding::Template:
                          case NamedBinding::Interface:
                            ident.resolved = ResolvedIdentifier{*binding};
                            return ExpressionResult::of_binding(*binding);

                          case NamedBinding::Argument:
                          case NamedBinding::Capture:
                          case NamedBinding::Field:
                          case NamedBinding::Narrowed:
                          case NamedBinding::Variable:
                            error_collector_.Add(
                                "refers to variable out of scope",
                                expression->meta);
                            return std::nullopt;
                        }
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
            Result lhs = RequireConcreteValue(binary.lhs, context);
            Result rhs = RequireConcreteValue(binary.rhs, context);

            if (!lhs || !rhs)
              return std::nullopt;

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
                auto unwrapped_type_id = type_context_.UnwrapOptional(
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
            Result lhs = RequireConcreteValue(assign.lhs, context);
            Result rhs = RequireConcreteValue(assign.rhs, context);

            if (!lhs.has_value() || !rhs.has_value())
              return std::nullopt;

            if (!lhs->binding.has_value() ||
                !(lhs->binding->kind == NamedBinding::Kind::Variable ||
                  lhs->binding->kind == NamedBinding::Kind::Field)) {
              error_collector_
                  .Add("can not assign to '" + lhs->binding->name.text,
                       assign.lhs->meta)
                  .WithNote("declared here", lhs->binding->name.metadata);
              return std::nullopt;
            }

            if (!type_context_.IsTypeSubsetOf(*rhs->type_id, *lhs->type_id)) {
              error_collector_.Add(
                  "mismatched assignment: " +
                      type_registry_.GetNameFromTypeId(*lhs->type_id) +
                      " vs. " + type_registry_.GetNameFromTypeId(*rhs->type_id),
                  expression->meta);
              return std::nullopt;
            }
            return rhs;
          },
          [&](MemberAccessExpression& member_access) {
            return HandleMemberAccess(member_access, context);
          },
          [&](ArrayAccessExpression& array_access) -> SemanticAnalyzer::Result {
            Result object = RequireConcreteValue(array_access.array, context);
            Result index = RequireConcreteValue(array_access.index, context);

            if (!object || !index)
              return std::nullopt;

            if (index->type_id != LiteralType::i32) {
              error_collector_.Add(
                  "index must be i32 but instead type is: " +
                      type_registry_.GetNameFromTypeId(*index->type_id),
                  array_access.index->meta);
              // Continue parsing to collect more errors.
            }

            // FIXME: Once templates exist we can narrot the type here.
            return ExpressionResult{LiteralType::Any};
          },
          [&](LogicExpression& logic) -> SemanticAnalyzer::Result {
            Result lhs = RequireConcreteValue(logic.lhs, context);
            Result rhs = RequireConcreteValue(logic.rhs, context);

            if (!lhs || !rhs)
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
              return ExpressionResult(*binding->realized_type_id);
            }
            CHECK(false) << "Failed to declare symbol for closure";
            return std::nullopt;
          },
          [&](PrefixUnaryExpression& prefix) -> SemanticAnalyzer::Result {
            Result operand = RequireConcreteValue(prefix.operand, context);
            // TODO: Check that `op` is valid for `operand`.
            return operand;
          },
          [&](PostfixUnaryExpression& postfix) -> SemanticAnalyzer::Result {
            Result operand = RequireConcreteValue(postfix.operand, context);
            // TODO: Check that `op` is valid for `operand`.
            return operand;
          },
          [&](TypeCastExpression& cast) -> SemanticAnalyzer::Result {
            Result result = RequireConcreteValue(cast.expr, context);
            if (!result.has_value())
              return std::nullopt;

            std::optional<TypeId> as_type =
                type_context_.GetTypeIdFor(cast.as_type);
            if (!as_type) {
              error_collector_.Add("unknown 'as' type", cast.as_type.metadata);
              return std::nullopt;
            }

            if (type_context_.UnwrapOptional(*as_type)) {
              error_collector_.Add("casts must be to non-nilable types",
                                   cast.as_type.metadata);
              return std::nullopt;
            }

            bool is_valid_cast =
                type_context_.IsTypeSubsetOf(as_type.value(), *result->type_id);

            // Allow explicit casts between i32 <=> Codepoint.
            is_valid_cast |= (as_type.value() == LiteralType::Codepoint &&
                              result->type_id == LiteralType::i32) ||
                             (result->type_id == LiteralType::Codepoint &&
                              as_type.value() == LiteralType::i32);

            // Allow explicit casts from i32 to f32. This is lossy for large
            // integers but common (most languages implicitly allow this cast).
            // Since Codepoints can be cast to i32 allow to f32 for consistency.
            is_valid_cast |= (as_type.value() == LiteralType::f32 &&
                              result->type_id == LiteralType::i32) ||
                             (as_type.value() == LiteralType::f32 &&
                              result->type_id == LiteralType::Codepoint);

            if (!is_valid_cast) {
              error_collector_.Add(
                  "Invalid type cast from " +
                      type_registry_.GetNameFromTypeId(*result->type_id) +
                      " to " + type_registry_.GetNameFromTypeId(*as_type),
                  cast.as_type.metadata);
              return std::nullopt;
            }

            TypeId new_type_id = as_type.value();
            if (cast.strategy == TypeCastStrategy::OPTIONAL) {
              new_type_id = type_context_.GetOptionalOf(as_type.value());
            }

            return ExpressionResult::with_type_override(new_type_id,
                                                        result->binding);
          },
          [&](OptionalChainExpression& optional_chain) -> Result {
            // OptionalChainExpression is a "pseudo-AST node" which represents
            // the END of a chain of ?. accesses (i.e. where to jump to in case
            // of Nil) and resolves to the final type wrapped as an Optional.
            Result result = RequireConcreteValue(optional_chain.root, context);
            if (!result.has_value())
              return std::nullopt;

            TypeId result_type_id = *result->type_id;
            if (!type_context_.UnwrapOptional(result_type_id)) {
              result_type_id = type_context_.GetOptionalOf(result_type_id);
            }

            ExpressionResult new_result = ExpressionResult::with_type_override(
                result_type_id, result->binding);
            new_result.narrowing_info = std::move(result->narrowing_info);
            return new_result;
          },
          [&](NilCoalescingExpression& coalescing) -> Result {
            Result lhs = RequireConcreteValue(coalescing.lhs, context);
            Result rhs = RequireConcreteValue(coalescing.rhs, context);

            if (!lhs || !rhs)
              return std::nullopt;

            if (type_context_.IsTypeNilable(*rhs->type_id)) {
              error_collector_.Add("right-hand side of ?? cannot be nilable",
                                   coalescing.rhs->meta);
              // Continue parsing to catch more errors...
            }

            std::optional<TypeId> lhs_type_id =
                type_context_.UnwrapOptional(*lhs->type_id);
            if (!lhs_type_id) {
              error_collector_.Add(
                  "left-hand side of ?? is not optional; operator is a no-op",
                  coalescing.lhs->meta);
              return std::nullopt;
            }

            return ExpressionResult{
                type_context_.GetUnionOf({*lhs_type_id, *rhs->type_id})};
          },
          [&](OptionalAccessExpression& optional_access) -> Result {
            Result result =
                RequireConcreteValue(optional_access.target, context);

            if (!result)
              return std::nullopt;

            TypeId type_id = *result->type_id;
            if (auto unwrapped = type_context_.UnwrapOptional(type_id)) {
              return ExpressionResult{unwrapped.value()};
            }

            error_collector_.Add("Unable to unwrap non-optional type: " +
                                     type_registry_.GetNameFromTypeId(type_id),
                                 optional_access.target->meta);
            return std::nullopt;
          },
          [&](TemplateInstantiationExpression& template_expr) -> Result {
            Result result =
                CheckExpression(template_expr.generic_target, context);

            if (!result.has_value())
              return std::nullopt;

            if (!result->binding.has_value() ||
                (result->binding->kind != NamedBinding::Struct &&
                 result->binding->kind != NamedBinding::Function)) {
              error_collector_.Add(".of() used on non-templated type",
                                   template_expr.generic_target->meta);
              return std::nullopt;
            }

            if (result->has_type_id()) {
              error_collector_
                  .Add("'" + result->binding->name.text + "' is not a template",
                       template_expr.generic_target->meta)
                  .WithNote("declared here", result->binding->name.metadata);
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
              return ExpressionResult::with_type_override(*type_id,
                                                          *result->binding);
            }

            return std::nullopt;
          },
      },
      expression->as);

  if (result) {
    expression->type_id = result->type_id;
  }
  return result;
}

void SemanticAnalyzer::TypeCheckCallArguments(
    const std::vector<std::optional<SpannedType>>& call_arugment_results,
    const std::vector<TypeId>& expected_argument_types,
    const Metadata& debug_metadata,
    std::optional<TypeId> variadic_type) {
  size_t supplied_argc = call_arugment_results.size();
  size_t expected_argc = expected_argument_types.size();

  if ((variadic_type && supplied_argc < expected_argc) ||
      (!variadic_type && supplied_argc != expected_argc)) {
    error_collector_.Add("Wrong number of arguments expected " +
                             std::to_string(expected_argc) + " but got " +
                             std::to_string(supplied_argc),
                         debug_metadata);
    return;
  }
  // If more arguments are supplied than expected, this is a variadic function
  // and any additional args do not need to be checked ("any" type).
  for (size_t i = 0; i < call_arugment_results.size(); ++i) {
    const auto& argument_result = call_arugment_results[i];
    const TypeId expected_type =
        (i < expected_argument_types.size() ? expected_argument_types[i]
                                            : *variadic_type);

    // Even if an argument expression does not parse correctly, continue on to
    // the next to try to collect as many errors as possible.
    if (!argument_result.has_value())
      continue;

    if (!type_context_.IsTypeSubsetOf(argument_result->type_id,
                                      expected_type)) {
      std::string expected_name =
          type_registry_.GetNameFromTypeId(expected_type);
      std::string actual_name =
          type_registry_.GetNameFromTypeId(argument_result->type_id);
      error_collector_.Add(
          "expected `" + expected_name + "` but got `" + actual_name + "`",
          argument_result->metadata);
    }
  }
}

SemanticAnalyzer::Result SemanticAnalyzer::TypeCheckCallExpr(
    CallExpression& call_expr,
    ExpressionResult callee_result,
    FunctionContext& context,
    Metadata debug_metadata) {
  // Ensure all arguments are type-checked regardless of the target.
  std::vector<std::optional<SpannedType>> argument_results;
  argument_results.reserve(call_expr.arguments.size());
  std::transform(
      call_expr.arguments.begin(), call_expr.arguments.end(),
      std::back_inserter(argument_results),
      [&](std::unique_ptr<Expression>& expr) -> std::optional<SpannedType> {
        if (auto result = RequireConcreteValue(expr, context))
          return SpannedType{*result->type_id, expr->meta};
        return std::nullopt;
      });

  std::optional<TypeId> callable_type_id = callee_result.type_id;

  if (!callable_type_id) {
    CHECK(callee_result.binding && callee_result.binding->symbol_id)
        << "SymbolId is required for templates";

    TypeResolver resolver(type_registry_, type_context_, error_collector_);

    std::vector<TypeId> deduced_bindings;
    if (resolver.Resolve(*callee_result.binding, argument_results,
                         deduced_bindings, call_expr.callee->meta)) {
      // // If there were no template variables to deduce then this Symbol is
      // // likely a method on a templated struct -- do not realize it here.
      // if (deduced_bindings.empty())
      //   return std::nullopt;

      callable_type_id =
          type_context_.GetTemplateOf(*callee_result.binding, deduced_bindings);
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
      call_expr.resolved = ResolvedCall{*callee_result.binding->symbol_id,
                                        symbol.declaration.function_kind};
    } else {
      call_expr.resolved = ResolvedCall{*callee_result.binding->symbol_id,
                                        FunctionKind::Anonymous};
    }

    TypeCheckCallArguments(argument_results, fn_type->arg_types, debug_metadata,
                           fn_type->variadic_type);

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
                           debug_metadata,
                           /*variadic_type=*/std::nullopt);
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

SemanticAnalyzer::Result SemanticAnalyzer::HandleMemberAccess(
    MemberAccessExpression& member_access,
    FunctionContext& context) {
  Result object_result = CheckExpression(member_access.object, context);
  if (!object_result)
    return std::nullopt;

  const auto& member_name = member_access.member_name;

  if (object_result->is_value() && object_result->has_type_id()) {
    TypeId type_id = *object_result->type_id;

    // Member access is only supported on structs
    const auto* const struct_type = type_registry_.GetType<StructType>(type_id);
    if (!struct_type) {
      error_collector_.Add("type `" +
                               type_registry_.GetNameFromTypeId(type_id) +
                               "` does not support member access",
                           member_access.object->meta);
      return std::nullopt;
    }

    if (auto binding = scope_manager_.FindBindingFor(
            member_name.text, ScopeManager::Current, struct_type->scope_id)) {
      if (binding->kind == NamedBinding::Field) {
        CHECK(binding->idx.has_value())
            << "member symbol must have an index for member access";
        member_access.resolved =
            ResolvedAccess{ResolvedAccess::Field{binding->idx.value()}};
      } else if (binding->kind == NamedBinding::Function) {
        CHECK(binding->symbol_id.has_value()) << "missing SymbolId on binding";
        member_access.resolved =
            ResolvedAccess{ResolvedAccess::Method{binding->symbol_id.value()}};
      }
      return ExpressionResult::of_binding(*binding);
    }

    error_collector_
        .Add("no member '" + member_name.text + "' found on instance of `" +
                 type_registry_.GetNameFromTypeId(type_id) + "`",
             member_name.metadata)
        .WithNote("declared here", struct_type->declaration.name.metadata);
    return std::nullopt;
  }

  if (object_result->is_type_ref() && object_result->binding &&
      object_result->binding->symbol_id) {
    SymbolId symbol_id = *object_result->binding->symbol_id;

    if (object_result->binding->kind != NamedBinding::Struct) {
      std::stringstream ss;
      ss << "binding of kind " << object_result->binding->kind
         << " does not support member access";
      error_collector_.Add(ss.str(), member_access.object->meta);
      return std::nullopt;
    }

    const auto* const struct_symbol =
        type_registry_.GetSymbol<StructSymbol>(symbol_id);
    CHECK(struct_symbol) << "StructSymbol not registered for id: " << symbol_id;

    if (auto binding = scope_manager_.FindBindingFor(
            member_name.text, ScopeManager::Current,
            struct_symbol->self_scope_id)) {
      // Filters out non-static methods (those that require `self` with kind
      // `Method`) and fields which are only accessible on an instance.
      if (binding->kind != NamedBinding::Function) {
        std::stringstream ss;
        ss << "binding of type '" << binding->kind
           << "' can only be accessed on an instance of "
           << struct_symbol->declaration.name.text;
        error_collector_.Add(ss.str(), member_access.member_name.metadata)
            .WithNote("declared here", binding->name.metadata);
        return std::nullopt;
      }

      CHECK(binding->symbol_id.has_value()) << "missing SymbolId on binding";
      member_access.resolved =
          ResolvedAccess{ResolvedAccess::Function{binding->symbol_id.value()}};

      return ExpressionResult::of_binding(*binding);
    }

    error_collector_
        .Add("no member '" + member_name.text + "' found on type `" +
                 struct_symbol->declaration.name.text + "`",
             member_access.object->meta)
        .WithNote("declared here", struct_symbol->declaration.name.metadata);
    return std::nullopt;
  }

  NOTREACHED() << "unhandled condition in MemberAccessExpression";
  return std::nullopt;
}

SemanticAnalyzer::Result SemanticAnalyzer::RequireConcreteValue(
    std::unique_ptr<Expression>& expression,
    FunctionContext& context) {
  Result result = CheckExpression(expression, context);

  if (!result)
    return std::nullopt;

  // Ensure it is an instance i.e. 123, x, fn foo().
  if (!result->is_value()) {
    if (result->binding.has_value()) {
      error_collector_
          .Add("expected value, but found '" + result->binding->name.text + "'",
               expression->meta)
          .WithNote("declared here", result->binding->name.metadata);
      return std::nullopt;
    }

    std::string type_name =
        (result->has_type_id()
             ? "'" + type_registry_.GetNameFromTypeId(*result->type_id) + "'"
             : "type");

    error_collector_.Add("expected value, but found " + type_name,
                         expression->meta);
    return std::nullopt;
  }

  // Ensure that the instance is fully instantiated (handles fn foo[T]() refs).
  if (!result->has_type_id()) {
    CHECK(result->binding.has_value()) << "MUST set TypeId and/or Binding";
    error_collector_
        .Add(result->binding->name.text +
                 " must be instantiated with template arguments "
                 "before it can be used as a value",
             expression->meta)
        .WithNote("declared here", result->binding->name.metadata);
    return std::nullopt;
  }

  return result;
}

std::ostream& operator<<(std::ostream& os, SemanticAnalyzer::Result result) {
  if (!result.has_value())
    return os << "_";

  os << "{ kind: ";
  if (result->is_value()) {
    os << "Value";
  } else if (result->is_type_ref()) {
    os << "TypeRef";
  }
  os << ", type_id: "
     << (result->type_id.has_value() ? std::to_string(*result->type_id) : "_");
  os << ", symbol_id: ";
  if (result->binding) {
    os << *result->binding;
  } else {
    os << "_";
  }
  return os << " }";
}
