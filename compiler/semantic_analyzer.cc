#include "compiler/semantic_analyzer.h"

#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <string_view>

#include "compiler/logging.h"
#include "compiler/printer.h"
#include "compiler/type_context.h"
#include "src/vm.h"

namespace {

class AutoScope {
 public:
  AutoScope(TypeContext& ctx,
            TypeContext::ScopeType type,
            FunctionDeclaration* fn = nullptr)
      : ctx_(ctx) {
    ctx_.EnterScope(type, fn);
  }
  ~AutoScope() { ctx_.ExitScope(); }

 private:
  TypeContext& ctx_;
};

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

using LiteralType = TypeContext::LiteralType;

}  // namespace

SemanticAnalyzer::SemanticAnalyzer(TypeContext& type_context,
                                   ErrorCollector& error_collector)
    : type_context_(type_context), error_collector_(error_collector) {}

void SemanticAnalyzer::Check(Block& block) {
  // Collect user-defined type names to ensure they're available for function
  // signatures, field types, etc. This also allows for recursive types (e.g. a
  // struct that has a field of its own type).
  std::vector<std::pair<Symbol, StructDeclaration*>> struct_decls;
  for (auto& statement : block.statements) {
    if (auto* struct_decl = std::get_if<StructDeclaration>(&statement->as)) {
      struct_decls.emplace_back(
          type_context_.DeclareStructSymbol(struct_decl->name), struct_decl);
    }
  }

  // Type check the full struct bodies (and functions) once all types are known.
  // Errors are logged from within `DefineStructType` and `DefineFunction`.
  for (auto& [self, decl] : struct_decls) {
    type_context_.DefineStructType(self.type_id, *decl, error_collector_);
  }
  for (auto& statement : block.statements) {
    if (auto* fn_decl = std::get_if<FunctionDeclaration>(&statement->as)) {
      type_context_.DefineFunction(*fn_decl, &error_collector_);
    }
  }

  for (auto& statement : block.statements) {
    CheckStatement(statement);
  }
}

void SemanticAnalyzer::CheckStatement(std::unique_ptr<Statement>& statement) {
  std::visit(
      Overloaded{
          [&](std::unique_ptr<Expression>& expr) { CheckExpression(expr); },
          [&](FunctionDeclaration& fn) { CheckFunctionBody(fn); },
          [&](ReturnStatement& ret) {
            Result return_result = CheckExpression(ret.value);

            if (!return_result.has_value())
              return;

            const auto& current_function = type_context_.GetCurrentFunction();
            CHECK(current_function.resolved.has_value());

            Symbol function_symbol = current_function.resolved->function_symbol;

            const FunctionType* function_type =
                type_context_.GetTypeInfo<FunctionType>(
                    function_symbol.type_id);
            CHECK(function_type)
                << "Invalid function type for symbol: " << function_symbol;
            TypeId expected_return_type = function_type->return_type;

            if (!type_context_.IsTypeSubsetOf(return_result->type_id,
                                              expected_return_type)) {
              error_collector_.Add(
                  "Returning " +
                      type_context_.GetNameFromTypeId(return_result->type_id) +
                      " from function with return type " +
                      type_context_.GetNameFromTypeId(expected_return_type),
                  statement->meta);
            }
          },
          [&](ThrowStatement& thr) { CheckExpression(thr.value); },
          [&](IfStatement& if_stmt) {
            CheckExpression(if_stmt.condition);

            {
              AutoScope _{type_context_, TypeContext::BlockScope};
              Check(if_stmt.then_body);
            }
            {
              AutoScope _{type_context_, TypeContext::BlockScope};
              Check(if_stmt.else_body);
            }
          },
          [&](WhileStatement& while_stmt) {
            CheckExpression(while_stmt.condition);
            {
              AutoScope _{type_context_, TypeContext::BlockScope};
              Check(while_stmt.body);
            }
          },
          // `break` and `continue` are single word statements.
          [&](const BreakStatement&) {},
          [&](const ContinueStatement&) {},
          [&](AssignStatement& assign) {
            // Check this is the first assignment in this scope with that name.
            if (type_context_.GetSymbolFor(assign.name, TypeContext::Current)) {
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

            Result value_result = CheckExpression(assign.value);
            if (!value_result.has_value())
              return;

            if (parsed_type_id.has_value()) {
              if (!type_context_.IsTypeSubsetOf(value_result->type_id,
                                                *parsed_type_id)) {
                error_collector_.Add(
                    "Assigning " + std::to_string(value_result->type_id) +
                        " to " + std::to_string(*parsed_type_id),
                    statement->meta);
                return;
              }
            } else {
              parsed_type_id = value_result->type_id;
            }

            // Register the variable's type within the current scope.
            Symbol symbol = type_context_.DeclareVariableSymbol(
                assign.name, parsed_type_id.value());
            assign.resolved = ResolvedIdentifier{symbol};
          },
          [&](StructDeclaration& struct_decl) {
            for (auto& [name, fn] : struct_decl.methods) {
              CheckFunctionBody(fn);
            }
          },
          [&](ImportStatement& import) { /*nothing to type check*/ },
      },
      statement->as);
}

SemanticAnalyzer::Result SemanticAnalyzer::CheckExpression(
    std::unique_ptr<Expression>& expression) {
  Result result = std::visit(
      Overloaded{
          [&](PrimaryExpression& primary) -> SemanticAnalyzer::Result {
            return std::visit(
                Overloaded{
                    [&](const StringLiteral&) -> SemanticAnalyzer::Result {
                      if (auto symbol = type_context_.GetSymbolFor("String"))
                        return ExpressionResult{symbol->type_id, symbol};

                      error_collector_.Add("unknown identifier: String",
                                           expression->meta);
                      return std::nullopt;
                    },
                    [&](Identifier& ident) -> SemanticAnalyzer::Result {
                      // Search within the current function scope for value.
                      auto symbol = type_context_.GetSymbolFor(
                          ident.name, TypeContext::ScopeToCheck::Function);
                      if (symbol) {
                        ident.resolved = ResolvedIdentifier{*symbol};
                        return ExpressionResult{symbol->type_id, *symbol};
                      }

                      // Fallback search to the parent function scope.
                      symbol = type_context_.GetSymbolFor(
                          ident.name, TypeContext::ScopeToCheck::Closure);
                      if (symbol) {
                        // Any value symbols found now must be captured.
                        if (symbol->kind == Symbol::Variable ||
                            symbol->kind == Symbol::Capture) {
                          FunctionDeclaration& fn =
                              type_context_.GetCurrentFunction();
                          fn.resolved->variables_to_capture.push_back(*symbol);

                          symbol = type_context_.DeclareCaptureSymbol(
                              ident.name, symbol->type_id);
                          fn.resolved->capture_arguments.push_back(*symbol);
                        }

                        ident.resolved = ResolvedIdentifier{*symbol};
                        return ExpressionResult{symbol->type_id, *symbol};
                      }

                      // Fallback seatch to ALL scopes for functions/structs.
                      symbol = type_context_.GetSymbolFor(
                          ident.name, TypeContext::ScopeToCheck::All);
                      if (symbol && (symbol->kind == Symbol::Function ||
                                     symbol->kind == Symbol::Struct)) {
                        ident.resolved = ResolvedIdentifier{*symbol};
                        return ExpressionResult{symbol->type_id, *symbol};
                      }

                      error_collector_.Add("unknown identifier: " + ident.name,
                                           expression->meta);
                      return std::nullopt;
                    },
                    [&](int32_t) -> SemanticAnalyzer::Result {
                      return ExpressionResult{LiteralType::i32};
                    },
                    [&](const CodepointLiteral& codepoint)
                        -> SemanticAnalyzer::Result {
                      return ExpressionResult{LiteralType::Codepoint};
                    },
                    [&](float) -> SemanticAnalyzer::Result {
                      return ExpressionResult{LiteralType::f32};
                    },
                    [&](bool) -> SemanticAnalyzer::Result {
                      return ExpressionResult{LiteralType::Bool};
                    },
                    [&](Nil) -> SemanticAnalyzer::Result {
                      return ExpressionResult{LiteralType::Nil};
                    }},
                primary.value);
          },
          [&](BinaryExpression& binary) -> SemanticAnalyzer::Result {
            Result lhs = CheckExpression(binary.lhs);
            Result rhs = CheckExpression(binary.rhs);

            if (!lhs.has_value() || !rhs.has_value())
              return std::nullopt;

            if (!TypeContext::IsTypeEquivalent(lhs->type_id, rhs->type_id) &&
                !((rhs->type_id == LiteralType::Nil &&
                   type_context_.IsTypeSubsetOf(rhs->type_id, lhs->type_id)) ||
                  (lhs->type_id == LiteralType::Nil &&
                   type_context_.IsTypeSubsetOf(lhs->type_id, rhs->type_id)))) {
              error_collector_.Add("LHS and RHS are not compatible",
                                   expression->meta);
              error_collector_.Add(
                  "LHS type is " +
                      type_context_.GetNameFromTypeId(lhs->type_id),
                  binary.lhs->meta);
              error_collector_.Add(
                  "But RHS type is " +
                      type_context_.GetNameFromTypeId(rhs->type_id),
                  binary.rhs->meta);
              return std::nullopt;
            }

            ResolvedBinary resolved{ResolvedBinary::Specialization::Number};
            if (lhs->type_id ==
                type_context_.GetTypeIdFor(ParsedType{"String"})) {
              resolved.specialization = ResolvedBinary::Specialization::String;
            } else if (lhs->type_id == LiteralType::Nil ||
                       rhs->type_id == LiteralType::Nil) {
              resolved.specialization = ResolvedBinary::Specialization::Nil;
            }
            binary.resolved = std::move(resolved);

            // Comparison operators will always generate a boolean
            if (binary.op == TokenKind::kCompareGt ||
                binary.op == TokenKind::kCompareLt ||
                binary.op == TokenKind::kCompareGe ||
                binary.op == TokenKind::kCompareLe ||
                binary.op == TokenKind::kCompareEq ||
                binary.op == TokenKind::kCompareNe)
              return ExpressionResult{LiteralType::Bool};

            return ExpressionResult{lhs->type_id};
          },
          [&](CallExpression& call_expr) -> SemanticAnalyzer::Result {
            Result callee_result = CheckExpression(call_expr.callee);

            // Short-circuit if the callee was invalid.
            if (!callee_result.has_value())
              return std::nullopt;

            if (!callee_result->symbol.has_value()) {
              LOG(ERROR) << "Only Symbols are callable!";
              return std::nullopt;
            }

            Result type_check_result = TypeCheckCallExpr(
                call_expr, callee_result->type_id,
                callee_result->symbol.value(), expression->meta);

            return type_check_result;
          },
          [&](AssignmentExpression& assign) -> SemanticAnalyzer::Result {
            Result lhs = CheckExpression(assign.lhs);
            Result rhs = CheckExpression(assign.rhs);

            if (!lhs.has_value() || !rhs.has_value())
              return std::nullopt;

            if (!TypeContext::IsTypeEquivalent(lhs->type_id, rhs->type_id) &&
                !(rhs->type_id == LiteralType::Nil &&
                  type_context_.IsTypeSubsetOf(rhs->type_id, lhs->type_id)))
              error_collector_.Add("Mismatched assignment", expression->meta);

            return lhs;
            return std::nullopt;
          },
          [&](MemberAccessExpression& member_access)
              -> SemanticAnalyzer::Result {
            Result object_result = CheckExpression(member_access.object);
            if (!object_result.has_value())
              return std::nullopt;

            if (auto* struct_type = type_context_.GetTypeInfo<StructType>(
                    object_result->type_id)) {
              const auto& member_name = member_access.member_name;
              if (auto it = struct_type->member_symbols.find(member_name);
                  it != struct_type->member_symbols.end()) {
                CHECK(it->second.idx.has_value())
                    << "Member symbol must have an index for member access";
                member_access.resolved = ResolvedAccess{it->second.idx.value()};
                return ExpressionResult{it->second.type_id, it->second};
              } else {
                LOG(ERROR) << "No member '" << member_name << "' on "
                           << struct_type->struct_declaration->name;
              }
            } else {
              LOG(ERROR) << "Only structs can have member access";
            }
            return std::nullopt;
          },
          [&](const ArrayAccessExpression& array_access)
              -> SemanticAnalyzer::Result { return std::nullopt; },
          [&](LogicExpression& logic) -> SemanticAnalyzer::Result {
            Result lhs = CheckExpression(logic.lhs);
            Result rhs = CheckExpression(logic.rhs);

            if (!lhs.has_value() || !rhs.has_value())
              return std::nullopt;

            if (!TypeContext::IsTypeEquivalent(lhs->type_id, rhs->type_id)) {
              error_collector_.Add("LHS and RHS are not compatible",
                                   expression->meta);
              error_collector_.Add(
                  "LHS type is " +
                      type_context_.GetNameFromTypeId(lhs->type_id),
                  logic.lhs->meta);
              error_collector_.Add(
                  "But RHS type is " +
                      type_context_.GetNameFromTypeId(rhs->type_id),
                  logic.rhs->meta);
              return std::nullopt;
            }

            return ExpressionResult{LiteralType::Bool};
          },
          [&](ClosureExpression& closure) -> SemanticAnalyzer::Result {
            if (auto symbol = type_context_.DefineFunction(closure.fn,
                                                           &error_collector_)) {
              CheckFunctionBody(closure.fn);
              return ExpressionResult{symbol->type_id, std::move(symbol)};
            }
            CHECK(false) << "Failed to declare symbol for closure";
            return std::nullopt;
          },
          [&](PrefixUnaryExpression& prefix) -> SemanticAnalyzer::Result {
            Result operand = CheckExpression(prefix.operand);
            // TODO: Check that `op` is valid for `operand`.
            return operand;
          },
          [&](PostfixUnaryExpression& postfix) -> SemanticAnalyzer::Result {
            Result operand = CheckExpression(postfix.operand);
            // TODO: Check that `op` is valid for `operand`.
            return operand;
          },
          [&](TypeCastExpression& cast) -> SemanticAnalyzer::Result {
            Result original_result = CheckExpression(cast.expr);
            std::optional<TypeId> as_type =
                type_context_.GetTypeIdFor(cast.as_type);
            if (!as_type) {
              error_collector_.Add("Unknown 'as' type", cast.as_type.metadata);
              return std::nullopt;
            }

            if (!type_context_.IsTypeSubsetOf(as_type.value(),
                                              original_result->type_id)) {
              error_collector_.Add(
                  "Invalid type cast from " +
                      type_context_.GetNameFromTypeId(
                          original_result->type_id) +
                      " to " + type_context_.GetNameFromTypeId(*as_type),
                  cast.as_type.metadata);
              return std::nullopt;
            }
            return ExpressionResult{as_type.value(), original_result->symbol};
          },
      },
      expression->as);
  expression->type = result.has_value() ? result->type_id : LiteralType::Void;
  return result;
}

void SemanticAnalyzer::CheckFunctionBody(FunctionDeclaration& fn) {
  if (!fn.body)
    return;

  {
    AutoScope _{type_context_, TypeContext::FunctionScope, &fn};
    for (const auto& [name, type] : fn.arguments) {
      std::optional<TypeId> type_id = type_context_.GetTypeIdFor(type);
      if (!type_id.has_value())
        return;

      fn.resolved->arguments.push_back(
          type_context_.DeclareVariableSymbol(name, type_id.value()));
    }

    Check(*fn.body);
  }
}

SemanticAnalyzer::Result SemanticAnalyzer::TypeCheckCallExpr(
    CallExpression& call_expr,
    TypeId callee_type_id,
    Symbol callee_symbol,
    Metadata debug_metadata) {
  if (const auto* const fn_type =
          type_context_.GetTypeInfo<FunctionType>(callee_type_id)) {
    // Account for the implicit "self" argument for method calls
    size_t extra_argc = 0;
    if (callee_symbol.kind == Symbol::Function) {
      CHECK(callee_symbol.idx.has_value())
          << "Function symbol must have an index for call resolution";
      const auto& fn_declaration =
          type_context_.GetFunctionDeclaration(callee_symbol.idx.value());

      if (fn_declaration.function_kind == FunctionKind::Method) {
        extra_argc += 1;
      }

      call_expr.resolved =
          ResolvedCall{callee_symbol.idx.value(), fn_declaration.function_kind};
    } else {
      call_expr.resolved =
          ResolvedCall{callee_symbol.idx.value(), FunctionKind::Anonymous};
    }

    size_t expected_argc = fn_type->arg_types.size();
    size_t actual_argc = call_expr.arguments.size() + extra_argc;

    if ((fn_type->is_variadic && actual_argc < expected_argc) ||
        (!fn_type->is_variadic && actual_argc != expected_argc)) {
      error_collector_.Add("Wrong number of arguments", debug_metadata);
    } else {
      for (size_t i = 0; i < call_expr.arguments.size(); ++i) {
        // Even if an argument expression does not parse correctly, continue
        // on to the next to try to collect as many errors as possible.
        if (Result argument_result = CheckExpression(call_expr.arguments[i])) {
          // If more arguments are supplied than expected, this is a variadic
          // function and any additional args should be handled as "any" type.
          TypeId expected_type = LiteralType::Any;
          if (i + extra_argc < expected_argc)
            expected_type = fn_type->arg_types[i + extra_argc];

          if (!type_context_.IsTypeSubsetOf(argument_result->type_id,
                                            expected_type)) {
            error_collector_.Add("Argument type mismatch",
                                 call_expr.arguments[i]->meta);
          }
        }
      }
    }
    // Even if the arguments can not be properly type checked we should
    // resolve to the return type to prevent cascading errors :^).
    return ExpressionResult{fn_type->return_type};
  }

  if (const auto* const struct_type =
          type_context_.GetTypeInfo<StructType>(callee_type_id)) {
    if (struct_type->struct_declaration->is_extern) {
      error_collector_.Add("extern structs have no constructor",
                           debug_metadata);
      return std::nullopt;
    }

    if (struct_type->field_types.size() != call_expr.arguments.size()) {
      error_collector_.Add("Wrong number of arguments", debug_metadata);
    } else {
      for (size_t i = 0; i < call_expr.arguments.size(); ++i) {
        if (Result argument_result = CheckExpression(call_expr.arguments[i])) {
          if (!type_context_.IsTypeSubsetOf(argument_result->type_id,
                                            struct_type->field_types[i])) {
            error_collector_.Add("Argument type mismatch",
                                 call_expr.arguments[i]->meta);
          }
        }
      }
    }
    call_expr.resolved = ResolvedCall{0, FunctionKind::Constructor};
    return ExpressionResult{callee_type_id};
  }

  error_collector_.Add("non-callable expression", debug_metadata);
  return std::nullopt;
}