// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/semantic_analyzer.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "compiler/error_collector.h"
#include "compiler/logging.h"
#include "compiler/symbol_binder.h"
#include "compiler/type_context.h"

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
  SymbolBinder(scope_manager_, type_registry_, type_context_, error_collector_)
      .Process(block);

  for (auto& statement : block.statements) {
    CheckStatement(statement, context);
  }

  std::vector<TypeContext::RealizedFunction> function_bodies =
      type_context_.GetRealizedFunctions();

  while (true) {
    if (function_bodies.empty())
      break;

    for (auto realized_function : function_bodies) {
      if (!realized_function.declaration.body)
        continue;

      scope_manager_.WithScope(realized_function.scope_id, [&]() {
        FunctionContext fn_context{{}, realized_function.return_type_id};
        Check(*realized_function.declaration.body, fn_context);
        realized_function.declaration.resolved->required_captures =
            std::move(fn_context.required_captures);
      });
    }

    function_bodies = type_context_.GetRealizedFunctions();
  }
}

void SemanticAnalyzer::CheckStatement(std::unique_ptr<Statement>& statement,
                                      FunctionContext& context) {
  auto check_expression = [this](std::unique_ptr<Expression>& expression,
                                 FunctionContext& context) {
    return ExpressionChecker(type_context_, scope_manager_, error_collector_,
                             type_registry_)
        .RequireConcreteValue(expression, context);
  };

  std::visit(
      Overloaded{
          [&](std::unique_ptr<Expression>& expr) {
            check_expression(expr, context);
          },
          [&](FunctionDeclaration& fn) {
            // Function bodies are checked only when a FunctionType is realized
          },
          [&](ReturnStatement& ret) {
            if (ret.value) {
              std::optional<ExpressionResult> result =
                  check_expression(ret.value, context);

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
          [&](ThrowStatement& thr) { check_expression(thr.value, context); },
          [&](IfStatement& if_stmt) {
            std::optional<ExpressionResult> result =
                check_expression(if_stmt.condition, context);

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
            std::optional<ExpressionResult> result =
                check_expression(while_stmt.condition, context);

            const auto narrowing_info = result
                                            ? result->narrowing_info
                                            : std::vector<ScopeNarrowingInfo>{};
            {
              AutoScope _{scope_manager_, ScopeManager::BlockScope, "while"};
              for (const auto& narrowing : narrowing_info) {
                scope_manager_.DeclareNarrowedBinding(narrowing.symbol,
                                                      narrowing.if_branch_type);
              }
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

            std::optional<ExpressionResult> result =
                check_expression(assign.value, context);
            if (!result.has_value()) {
              NamedBinding binding = scope_manager_.DeclareVariableBinding(
                  assign.name, TypeRegistry::Error);
              return;
            }

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
                parsed_type_id = TypeRegistry::Error;
              }
            } else {
              parsed_type_id = result->type_id;
            }

            // Register the variable's type within the current scope.
            NamedBinding binding = scope_manager_.DeclareVariableBinding(
                assign.name, parsed_type_id.value());
            CHECK(!assign.resolved) << "Identifier was previously resolved";
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
