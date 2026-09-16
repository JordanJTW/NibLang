// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/semantic_analyzer.h"

#include <ranges>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "compiler/error_collector.h"
#include "compiler/expression_checker.h"
#include "compiler/logging.h"
#include "compiler/symbol_binder.h"
#include "compiler/type_context.h"
#include "compiler/type_resolver.h"

namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

using LiteralType = TypeRegistry::LiteralType;

std::ostream& operator<<(std::ostream& os, const FlowResult::Status& status) {
  switch (status) {
    case FlowResult::Status::Fallthrough:
      return os << "Fallthrough";
    case FlowResult::Status::Terminate:
      return os << "Terminate";
  }
  return os << "Unknown FlowResult::Status";
}
}  // namespace

SemanticAnalyzer::SemanticAnalyzer(TypeContext& type_context,
                                   ScopeManager& scope_manager,
                                   ErrorCollector& error_collector,
                                   TypeRegistry& type_registry)
    : type_context_(type_context),
      scope_manager_(scope_manager),
      error_collector_(error_collector),
      type_registry_(type_registry) {}

FlowResult SemanticAnalyzer::Check(Block& block,
                                   FunctionContext& context,
                                   const NarrowedBindings& narrowed_bindings,
                                   std::optional<LoopContext*> loop_context) {
  SymbolBinder(scope_manager_, type_registry_, type_context_, error_collector_)
      .Process(block);

  FlowResult exit_result{FlowResult::Status::Fallthrough, narrowed_bindings};
  for (auto& statement : block.statements) {
    // `exit_status` should be terminated exactly once per flow.
    if (exit_result.is_terminated()) {
      error_collector_.Add("unreachable statement", statement->meta);
      continue;
    }

    FlowResult result =
        Check(statement, context, *exit_result.narrowed_bindings, loop_context);
    if (result.narrowed_bindings)
      exit_result.narrowed_bindings = result.narrowed_bindings;

    exit_result.status = result.status;
  }

  std::vector<TypeContext::RealizedFunction> function_bodies =
      type_context_.GetRealizedFunctions();

  while (true) {
    if (function_bodies.empty())
      break;

    for (auto [scope_id, declaration, return_type_id] : function_bodies) {
      if (!declaration.body)
        continue;

      scope_manager_.WithScope(scope_id, [&]() {
        FunctionContext fn_context{{}, return_type_id};
        FlowResult result =
            Check(*declaration.body, fn_context, NarrowedBindings{});

        if (result.is_fallthrough()) {
          if (return_type_id == LiteralType::Unit) {
            declaration.resolved->should_insert_unit_return = true;
          } else {
            error_collector_.Add(
                "function with return type " +
                    type_registry_.GetNameFromTypeId(return_type_id) +
                    " does not return on all paths",
                declaration.name.metadata);
          }
        }

        declaration.resolved->required_captures =
            std::move(fn_context.required_captures);
      });
    }

    function_bodies = type_context_.GetRealizedFunctions();
  }

  return exit_result;
}

FlowResult SemanticAnalyzer::Check(const std::unique_ptr<Statement>& statement,
                                   FunctionContext& function_context,
                                   const NarrowedBindings& existing_bindings,
                                   std::optional<LoopContext*> loop_context) {
  auto check_expression = [&](std::unique_ptr<Expression>& expression) {
    auto result =
        ExpressionChecker(scope_manager_, type_context_, type_registry_,
                          existing_bindings, function_context.required_captures,
                          error_collector_)
            .RequireConcreteValue(expression);

    if (!result)
      return result;

    auto intersect_with_current = [&](const NarrowedBindings& branch_bindings) {
      NarrowedBindings merged = existing_bindings;
      for (const auto& [id, type_id] : branch_bindings) {
        if (merged.contains(id)) {
          merged[id] = type_context_.GetIntersectionOf({merged[id], type_id});
        } else {
          merged[id] = type_id;
        }
      }
      return merged;
    };

    result->true_bindings = intersect_with_current(result->true_bindings);
    result->false_bindings = intersect_with_current(result->false_bindings);
    return result;
  };

  return std::visit(
      Overloaded{
          [&](std::unique_ptr<Expression>& expr) {
            auto result = check_expression(expr);

            std::optional<NarrowedBindings> updated_bindings;
            if (result && !result->true_bindings.empty())
              updated_bindings = result->true_bindings;

            return FlowResult{FlowResult::Status::Fallthrough,
                              updated_bindings};
          },
          [&](ReturnStatement& ret) {
            if (ret.value) {
              if (auto result = check_expression(ret.value)) {
                if (!type_context_.IsTypeSubsetOf(
                        *result->type_id, function_context.return_type_id)) {
                  error_collector_.Add(
                      "Returning " +
                          type_registry_.GetNameFromTypeId(*result->type_id) +
                          " from function with return type " +
                          type_registry_.GetNameFromTypeId(
                              function_context.return_type_id),
                      statement->meta);
                }
              }
            } else {
              if (!type_context_.IsTypeSubsetOf(
                      TypeRegistry::Unit, function_context.return_type_id)) {
                error_collector_.Add(
                    "Returning `Unit` from function with return type " +
                        type_registry_.GetNameFromTypeId(
                            function_context.return_type_id),
                    statement->meta);
              }
            }
            return FlowResult{FlowResult::Status::Terminate};
          },
          [&](ThrowStatement& thr) {
            check_expression(thr.value);
            // TODO: Should `throw` be the same as a Return?
            return FlowResult{FlowResult::Status::Terminate};
          },
          [&](IfStatement& if_stmt) {
            auto result = check_expression(if_stmt.condition);

            NarrowedBindings true_bindings =
                result ? result->true_bindings : existing_bindings;
            NarrowedBindings false_bindings =
                result ? result->false_bindings : existing_bindings;

            scope_manager_.EnterScope(ScopeManager::BlockScope, "if");
            FlowResult flow_if = Check(if_stmt.then_body, function_context,
                                       true_bindings, loop_context);
            scope_manager_.ExitScope();

            scope_manager_.EnterScope(ScopeManager::BlockScope, "else");
            FlowResult flow_else = Check(if_stmt.else_body, function_context,
                                         false_bindings, loop_context);
            scope_manager_.ExitScope();

            if (flow_if.is_terminated() && flow_else.is_terminated())
              return FlowResult{FlowResult::Status::Terminate};

            if (flow_if.is_terminated())
              return flow_else;

            if (flow_else.is_terminated())
              return flow_if;

            const auto& if_exit =
                flow_if.narrowed_bindings.value_or(true_bindings);
            const auto& else_exit =
                flow_else.narrowed_bindings.value_or(false_bindings);

            return FlowResult{
                FlowResult::Status::Fallthrough,
                UnionBindings(if_exit, else_exit, existing_bindings)};
          },
          [&](WhileStatement& while_stmt) {
            auto result = check_expression(while_stmt.condition);

            LoopContext current_context{/*initial_bindings=*/existing_bindings};
            FlowResult body_flow =
                Check(while_stmt.body, function_context,
                      result ? result->true_bindings : existing_bindings,
                      &current_context);

            if (body_flow.is_terminated() && !current_context.contains_break) {
              return FlowResult{FlowResult::Status::Terminate};
            }

            auto exit_bindings =
                result ? result->false_bindings : existing_bindings;
            if (current_context.break_bindings) {
              exit_bindings =
                  UnionBindings(exit_bindings, *current_context.break_bindings,
                                existing_bindings);
            }

            return FlowResult{FlowResult::Status::Fallthrough,
                              std::move(exit_bindings)};
          },
          [&](const BreakStatement&) {
            if (!loop_context.has_value()) {
              error_collector_.Add("`break` must exist within a loop",
                                   statement->meta);
              // `break` outside a loop means nothing, just ignore it
              return FlowResult{FlowResult::Status::Fallthrough};
            }

            if (!(*loop_context)->break_bindings.has_value()) {
              (*loop_context)->break_bindings = existing_bindings;
            } else {
              (*loop_context)->break_bindings = UnionBindings(
                  (*loop_context)->break_bindings.value(), existing_bindings,
                  (*loop_context)->initial_bindings);
            }

            (*loop_context)->contains_break = true;
            return FlowResult{FlowResult::Status::Terminate};
          },
          [&](const ContinueStatement&) {
            return FlowResult{FlowResult::Status::Terminate};
          },
          [&](AssignStatement& assign) {
            // Ensure assignment expression's type matches the declared type (if
            // given, otherwise the variable's type is deduced from the value).
            std::optional<TypeId> parsed_type_id;
            if (assign.type.has_value()) {
              parsed_type_id = type_context_.GetTypeIdFor(*assign.type);
            }

            auto result = check_expression(assign.value);
            if (!result.has_value()) {
              NamedBinding binding = scope_manager_.DeclareVariableBinding(
                  assign.name, TypeRegistry::Error);
              return FlowResult{FlowResult::Status::Fallthrough};
            }

            if (parsed_type_id.has_value()) {
              if (!type_context_.IsTypeSubsetOf(*result->type_id,
                                                *parsed_type_id)) {
                std::string expected_type =
                    type_registry_.GetNameFromTypeId(*parsed_type_id);

                error_collector_.Add(
                    "expected " +
                        type_registry_.GetNameFromTypeId(*parsed_type_id) +
                        ", but found " +
                        type_registry_.GetNameFromTypeId(*result->type_id),
                    assign.type->metadata);
                // Intentional fallthrough to prevent error cascades.
              }
            } else {
              parsed_type_id = result->type_id;
            }

            // Register the variable's type within the current scope.
            NamedBinding binding = scope_manager_.DeclareVariableBinding(
                assign.name, parsed_type_id.value());
            CHECK(!assign.resolved) << "Identifier was previously resolved";
            assign.resolved = ResolvedIdentifier{binding};
            return FlowResult{FlowResult::Status::Fallthrough};
          },
          // The following top-level declarations are handled within
          // SymbolBinder (except ImportStatement in main()).
          [&](const StructDeclaration&) {
            return FlowResult{FlowResult::Status::Fallthrough};
          },
          [&](const FunctionDeclaration&) {
            return FlowResult{FlowResult::Status::Fallthrough};
          },
          [&](const ImportStatement&) {
            return FlowResult{FlowResult::Status::Fallthrough};
          },
          [&](const TypeAliasStatement&) {
            return FlowResult{FlowResult::Status::Fallthrough};
          },
          [&](const InterfaceDeclaration&) {
            return FlowResult{FlowResult::Status::Fallthrough};
          },
      },
      statement->as);
}

NarrowedBindings SemanticAnalyzer::UnionBindings(const NarrowedBindings& left,
                                                 const NarrowedBindings& right,
                                                 const NarrowedBindings& base) {
  std::unordered_set<BindingId> vars;
  for (const auto& id : left | std::views::keys)
    vars.insert(id);
  for (const auto& id : right | std::views::keys)
    vars.insert(id);

  NarrowedBindings result = base;
  for (BindingId id : vars) {
    bool left_has = left.contains(id);
    bool right_has = right.contains(id);

    if (!base.contains(id)) {
      // If the only information we have is from one branch, then the other
      // would have to assume that the variable could be Any which would
      // overly broaden the variable. In that case it's better to simply not try
      // to narrow and fallback to the originally declared type.
      if (left_has && right_has) {
        result[id] = type_context_.GetUnionOf({left.at(id), right.at(id)});
      }
    } else {
      const auto& t1 = left_has ? left.at(id) : base.at(id);
      const auto& t2 = right_has ? right.at(id) : base.at(id);
      result[id] = type_context_.GetUnionOf({t1, t2});
    }
  }

  return result;
}
