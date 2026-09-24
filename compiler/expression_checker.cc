// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/expression_checker.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "compiler/error_collector.h"
#include "compiler/scope_manager.h"
#include "compiler/type_context.h"
#include "compiler/type_registry.h"
#include "compiler/type_resolver.h"
#include "compiler/type_rewriter.h"

namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

using LiteralType = ::TypeRegistry::LiteralType;

// Builds a mapping between generic template variable TypeIds and the concrete
// arguments assigned to them in `type` i.e. Array[T] => Array[i32].
void PopulateInstanceTypes(const StructType& type,
                           SubstitutionMap& substitution_map) {
  for (size_t i = 0; i < type.instance_template_type_ids.size(); ++i) {
    substitution_map[type.symbol->template_variable_type_ids[i]] =
        type.instance_template_type_ids[i];
  }
}

}  // namespace

ExpressionChecker::ExpressionChecker(
    ScopeManager& scope_manager,
    TypeContext& type_context,
    TypeRegistry& type_registry,
    NarrowedBindings narrowed_bindings,
    std::vector<NamedBinding>& required_captures,
    ErrorCollector& error_collector)
    : scope_manager_(scope_manager),
      type_context_(type_context),
      type_registry_(type_registry),
      narrowed_bindings_(std::move(narrowed_bindings)),
      required_captures_(required_captures),
      error_collector_(error_collector),
      type_resolver_(type_registry_, type_context_, error_collector_) {}

std::optional<ExpressionResult> ExpressionChecker::CheckChain(
    std::unique_ptr<Expression>& root_expression,
    std::optional<SpannedType> hint_return_type) {
  auto result = RequireValue(root_expression, hint_return_type);

  if (!result)
    return std::nullopt;

  if (type_registry_.HasPlaceholderTypes(result->type_id)) {
    error_collector_.Add("unable to infer types in " +
                             type_registry_.GetNameFromTypeId(result->type_id),
                         root_expression->meta);
    return std::nullopt;
  }

  return result;
}

std::optional<ExpressionResult> ExpressionChecker::Check(
    std::unique_ptr<Expression>& expression,
    std::optional<SpannedType> hint_return_type) {
  std::optional<ExpressionResult> result = std::visit(
      Overloaded{
          [&](PrimaryExpression& primary) -> std::optional<ExpressionResult> {
            auto result = HandlePrimary(primary, expression->meta);
            if (!result || result->type_id == LiteralType::Error)
              return std::nullopt;
            return result;
          },
          [&](BinaryExpression& binary) -> std::optional<ExpressionResult> {
            auto lhs = RequireValue(binary.lhs);
            auto rhs = RequireValue(binary.rhs);

            if (!lhs || !rhs)
              return std::nullopt;

            if (lhs->type_id != rhs->type_id &&
                !((rhs->type_id == LiteralType::Nil &&
                   type_context_.IsTypeSubsetOf(rhs->type_id, lhs->type_id)) ||
                  (lhs->type_id == LiteralType::Nil &&
                   type_context_.IsTypeSubsetOf(lhs->type_id, rhs->type_id)))) {
              error_collector_
                  .Add("LHS and RHS are not compatible", expression->meta)
                  .WithNote("LHS type is " +
                                type_registry_.GetNameFromTypeId(lhs->type_id),
                            binary.lhs->meta)
                  .WithNote("But RHS type is " +
                                type_registry_.GetNameFromTypeId(rhs->type_id),
                            binary.rhs->meta);
              return std::nullopt;
            }

            ResolvedBinary resolved{ResolvedBinary::Specialization::Number};
            NarrowedBindings true_bindings, false_bindings;
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
                auto unwrapped_type_id =
                    type_context_.UnwrapOptional(symbol_to_narrow->type_id);

                // This should NEVER be hit. See compatibility check above.
                CHECK(unwrapped_type_id)
                    << "Non-optional type cannot be narrowed from Nil";

                TypeId if_branch_type = unwrapped_type_id.value();
                TypeId else_branch_type = LiteralType::Nil;
                if (binary.op != TokenKind::kCompareNe) {
                  std::swap(if_branch_type, else_branch_type);
                }

                true_bindings[symbol_to_narrow->binding_id] = if_branch_type;
                false_bindings[symbol_to_narrow->binding_id] = else_branch_type;
              }
            }

            CHECK(!binary.resolved)
                << "BinaryExpression was previously resolved";
            binary.resolved = resolved;

            // Comparison operators will always generate a boolean
            if (binary.op == TokenKind::kCompareGt ||
                binary.op == TokenKind::kCompareLt ||
                binary.op == TokenKind::kCompareGe ||
                binary.op == TokenKind::kCompareLe ||
                binary.op == TokenKind::kCompareEq ||
                binary.op == TokenKind::kCompareNe) {
              ExpressionResult result(LiteralType::Bool);
              result.true_bindings = std::move(true_bindings);
              result.false_bindings = std::move(false_bindings);
              return result;
            }

            return ExpressionResult(lhs->type_id);
          },
          [&](CallExpression& call_expr) -> std::optional<ExpressionResult> {
            auto callee_result = Check(call_expr.callee);

            // Short-circuit if the callee was invalid.
            if (!callee_result.has_value())
              return std::nullopt;

            auto type_check_result =
                TypeCheckCallExpr(call_expr, callee_result.value(),
                                  hint_return_type, expression->meta);

            return type_check_result;
          },
          [&](AssignmentExpression& assign) -> std::optional<ExpressionResult> {
            auto lhs = Check(assign.lhs);
            auto rhs = RequireValue(assign.rhs);

            if (!lhs.has_value() || !rhs.has_value())
              return std::nullopt;

            if (!lhs->binding || !lhs->binding->IsVariable()) {
              if (lhs->binding) {
                error_collector_
                    .Add("cannot assign to '" + lhs->binding->name.text + "'",
                         assign.lhs->meta)
                    .WithNote("declared here", lhs->binding->name.metadata);
                return std::nullopt;
              }

              error_collector_.Add("cannot assign to this expression",
                                   assign.lhs->meta);
              return std::nullopt;
            }

            // Always use the `binding` type directly since it describes the
            // type of the slot and not whatever it might have been narrowed to.
            TypeId storage_type_id =
                lhs->storage_type_id.value_or(lhs->type_id);
            if (!type_context_.IsTypeSubsetOf(rhs->type_id, storage_type_id)) {
              error_collector_.Add(
                  "expected " +
                      type_registry_.GetNameFromTypeId(storage_type_id) +
                      ", but found " +
                      type_registry_.GetNameFromTypeId(rhs->type_id),
                  expression->meta);
              return std::nullopt;
            }
            if (lhs->binding->type_id != rhs->type_id) {
              rhs->true_bindings[lhs->binding->binding_id] = rhs->type_id;
            }
            return rhs;
          },
          [&](MemberAccessExpression& member_access) {
            return HandleMemberAccess(member_access);
          },
          [&](ArrayAccessExpression& array_access)
              -> std::optional<ExpressionResult> {
            auto object = RequireValue(array_access.array);
            auto index = RequireValue(array_access.index);

            if (!object || !index)
              return std::nullopt;

            if (index->type_id != LiteralType::i32) {
              error_collector_.Add(
                  "index must be i32 but instead type is: " +
                      type_registry_.GetNameFromTypeId(index->type_id),
                  array_access.index->meta);
              // Continue parsing to collect more errors.
            }

            // FIXME: Once templates exist we can narrow the type here.
            return ExpressionResult{LiteralType::Any};
          },
          [&](LogicExpression& logic) -> std::optional<ExpressionResult> {
            auto lhs = RequireValue(logic.lhs);
            auto rhs = RequireValue(logic.rhs);

            if (!lhs || !rhs)
              return std::nullopt;

            if (lhs->type_id != rhs->type_id) {
              error_collector_
                  .Add("LHS and RHS are not compatible", expression->meta)
                  .WithNote("LHS type is " +
                                type_registry_.GetNameFromTypeId(lhs->type_id),
                            logic.lhs->meta)
                  .WithNote("But RHS type is " +
                                type_registry_.GetNameFromTypeId(rhs->type_id),
                            logic.rhs->meta);
              return std::nullopt;
            }

            NarrowedBindings true_bindings, false_bindings;
            // AND requires both sides to be true. `true_bindings` uses the
            // union of keys (all are known to be correct), and the intersection
            // of types for any overlapping keys to satisfy both constraints
            // simultaneously. `false_bindings` use the opposite.
            if (logic.kind == LogicExpression::Kind::AND) {
              true_bindings = lhs->true_bindings;  // Conflicts overwritten
              for (const auto& [id, type_id] : rhs->true_bindings) {
                if (lhs->true_bindings.contains(id)) {
                  true_bindings[id] = type_context_.GetIntersectionOf(
                      {type_id, lhs->true_bindings[id]});
                } else {
                  true_bindings[id] = type_id;
                }
              }

              for (const auto& [id, type_id] : lhs->false_bindings) {
                if (rhs->false_bindings.contains(id)) {
                  false_bindings[id] = type_context_.GetUnionOf(
                      {type_id, rhs->false_bindings[id]});
                }
              }
            }
            // OR requires at least one side to succeed. `true_bindings` uses
            // the intersection of keys (since we don't know which branch
            // executed), and the union of types for overlapping keys (as either
            // type may be valid). `false_bindings` use the opposite.
            else if (logic.kind == LogicExpression::Kind::OR) {
              for (const auto& [id, type_id] : lhs->true_bindings) {
                if (rhs->true_bindings.contains(id)) {
                  true_bindings[id] = type_context_.GetUnionOf(
                      {type_id, rhs->true_bindings[id]});
                }
              }

              false_bindings = lhs->true_bindings;  // Conflicts overwritten
              for (const auto& [id, type_id] : rhs->false_bindings) {
                if (lhs->false_bindings.contains(id)) {
                  false_bindings[id] = type_context_.GetIntersectionOf(
                      {type_id, lhs->false_bindings[id]});
                } else {
                  false_bindings[id] = type_id;
                }
              }
            } else {
              NOTREACHED() << "Unhandled logic expression!";
            }

            ExpressionResult result(LiteralType::Bool);
            result.true_bindings = std::move(true_bindings);
            result.false_bindings = std::move(false_bindings);
            return result;
          },
          [&](ClosureExpression& closure) -> std::optional<ExpressionResult> {
            SymbolId symbol_id = type_registry_.NewFunctionSymbol(closure.fn);
            closure.fn.resolved =
                ResolvedFunction{.function_symbol_id = symbol_id};

            auto& symbol =
                type_registry_.GetSymbolChecked<FunctionSymbol>(symbol_id);
            if (auto instance = type_context_.DeclareFunctionType(
                    symbol, TypeContext::CheckFunctionBody::YES)) {
              return ExpressionResult(instance->type_id);
            }
            CHECK(false) << "Failed to declare symbol for closure";
            return std::nullopt;
          },
          [&](PrefixUnaryExpression& prefix)
              -> std::optional<ExpressionResult> {
            auto operand = RequireValue(prefix.operand);
            // TODO: Check that `op` is valid for `operand`.
            return operand;
          },
          [&](PostfixUnaryExpression& postfix)
              -> std::optional<ExpressionResult> {
            auto operand = RequireValue(postfix.operand);
            // TODO: Check that `op` is valid for `operand`.
            return operand;
          },
          [&](TypeCastExpression& cast) -> std::optional<ExpressionResult> {
            auto result = RequireValue(cast.expr);
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
                type_context_.IsTypeSubsetOf(as_type.value(), result->type_id);

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
                      type_registry_.GetNameFromTypeId(result->type_id) +
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
          [&](OptionalChainExpression& optional_chain)
              -> std::optional<ExpressionResult> {
            // OptionalChainExpression is a "pseudo-AST node" which represents
            // the END of a chain of ?. accesses (i.e. where to jump to in case
            // of Nil) and resolves to the final type wrapped as an Optional.
            auto result = RequireValue(optional_chain.root);
            if (!result.has_value())
              return std::nullopt;

            TypeId result_type_id = result->type_id;
            if (!type_context_.UnwrapOptional(result_type_id)) {
              result_type_id = type_context_.GetOptionalOf(result_type_id);
            }

            return ExpressionResult::with_type_override(result_type_id,
                                                        result->binding);
          },
          [&](NilCoalescingExpression& coalescing)
              -> std::optional<ExpressionResult> {
            auto lhs = RequireValue(coalescing.lhs);
            auto rhs = RequireValue(coalescing.rhs);

            if (!lhs || !rhs)
              return std::nullopt;

            if (type_context_.IsTypeNilable(rhs->type_id)) {
              error_collector_.Add("right-hand side of ?? cannot be nilable",
                                   coalescing.rhs->meta);
              // Continue parsing to catch more errors...
            }

            std::optional<TypeId> lhs_type_id =
                type_context_.UnwrapOptional(lhs->type_id);
            if (!lhs_type_id) {
              error_collector_.Add(
                  "left-hand side of ?? is not optional; operator is a no-op",
                  coalescing.lhs->meta);
              return std::nullopt;
            }

            return ExpressionResult{
                type_context_.GetUnionOf({*lhs_type_id, rhs->type_id})};
          },
          [&](OptionalAccessExpression& optional_access)
              -> std::optional<ExpressionResult> {
            auto result = RequireValue(optional_access.target);

            if (!result)
              return std::nullopt;

            TypeId type_id = result->type_id;
            if (auto unwrapped = type_context_.UnwrapOptional(type_id)) {
              return ExpressionResult{unwrapped.value()};
            }

            error_collector_.Add("Unable to unwrap non-optional type: " +
                                     type_registry_.GetNameFromTypeId(type_id),
                                 optional_access.target->meta);
            return std::nullopt;
          },
          [&](TemplateInstantiationExpression& template_expr)
              -> std::optional<ExpressionResult> {
            auto result = Check(template_expr.generic_target);

            if (!result.has_value())
              return std::nullopt;

            if (!result->binding ||
                (result->binding->kind != NamedBinding::Struct &&
                 result->binding->kind != NamedBinding::Function)) {
              error_collector_.Add(".of() used on non-templated type",
                                   template_expr.generic_target->meta);
              return std::nullopt;
            }

            if (!type_registry_.HasPlaceholderTypes(result->type_id)) {
              error_collector_.Add(
                  type_registry_.GetNameFromTypeId(result->type_id) +
                      " is a concrete type",
                  template_expr.generic_target->meta);
              return std::nullopt;
            }

            auto concrete_type_id = type_context_.GetTemplateOf(
                result->binding->GetSymbolId(), template_expr.template_types,
                expression->meta);

            if (!concrete_type_id)  // Errors logged in `GetTemplateOf()`
              return std::nullopt;

            return ExpressionResult::with_type_override(*concrete_type_id,
                                                        result->binding);
          },
      },
      expression->as);

  if (result) {
    expression->type_id = result->type_id;
  }
  return result;
}

void ExpressionChecker::TypeCheckCallArguments(
    const std::vector<std::optional<SpannedType>>& call_argument_results,
    const std::vector<TypeId>& expected_argument_types,
    const Metadata& debug_metadata,
    std::optional<TypeId> variadic_type) {
  size_t supplied_argc = call_argument_results.size();
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
  for (size_t i = 0; i < call_argument_results.size(); ++i) {
    const auto& argument_result = call_argument_results[i];

    const TypeId expected_type =
        (i < expected_argument_types.size() ? expected_argument_types[i]
                                            : *variadic_type);

    // Even if an argument expression does not parse correctly, continue on to
    // the next to try to collect as many errors as possible.
    if (!argument_result.has_value())
      continue;

    if (!type_resolver_.Resolve(expected_type, argument_result->type_id,
                                argument_result->metadata) &&
        !type_context_.IsTypeSubsetOf(argument_result->type_id,
                                      expected_type)) {
      TypeId resolved_expected = type_resolver_.Prune(expected_type);
      TypeId resolved_actual = type_resolver_.Prune(argument_result->type_id);

      std::string expected_name =
          type_registry_.GetNameFromTypeId(resolved_expected);
      std::string actual_name =
          type_registry_.GetNameFromTypeId(resolved_actual);

      error_collector_.Add(
          "expected " + expected_name + ", but got " + actual_name,
          argument_result->metadata);
    }
  }
}

std::optional<ExpressionResult> ExpressionChecker::TypeCheckCallExpr(
    CallExpression& call_expr,
    ExpressionResult callee_result,
    std::optional<SpannedType> hint_return_type,
    Metadata debug_metadata) {
  // Validate constructor calls BEFORE any type-deduction for better errors.
  if (callee_result.is_type_ref() &&
      callee_result.binding->kind == NamedBinding::Struct) {
    auto* struct_symbol = type_registry_.GetSymbol<StructSymbol>(
        callee_result.binding->GetSymbolId());
    if (struct_symbol->declaration.kind != StructDeclaration::Structure) {
      error_collector_.Add(
          "`opaque` or `interface` structs have no constructor",
          debug_metadata);
      return std::nullopt;
    }
  }

  // Ensure all arguments are type-checked regardless of the target.
  std::vector<std::optional<SpannedType>> argument_results;
  argument_results.reserve(call_expr.arguments.size());
  std::transform(
      call_expr.arguments.begin(), call_expr.arguments.end(),
      std::back_inserter(argument_results),
      [&](std::unique_ptr<Expression>& expr) -> std::optional<SpannedType> {
        if (auto result = RequireValue(expr))
          return SpannedType{result->type_id, expr->meta};
        return std::nullopt;
      });

  TypeId callable_type_id = callee_result.type_id;

  if (const auto* const fn_type =
          type_registry_.GetType<FunctionType>(callable_type_id)) {
    if (callee_result.binding &&
        callee_result.binding->kind == NamedBinding::Function) {
      const auto& symbol = type_registry_.GetSymbolChecked<FunctionSymbol>(
          callee_result.binding->GetSymbolId());
      CHECK(!call_expr.resolved) << "CallExpression was previously resolved";
      call_expr.resolved = ResolvedCall{callee_result.binding->GetSymbolId(),
                                        symbol.RequiresVirtualDispatch()
                                            ? FunctionKind::Virtual
                                            : symbol.declaration.function_kind};
    } else {
      CHECK(!call_expr.resolved) << "CallExpression was previously resolved";
      // Assume a bound closure on the stack so no `target_symbol_id` is needed.
      call_expr.resolved = ResolvedCall{0, FunctionKind::Anonymous};
    }

    TypeCheckCallArguments(argument_results, fn_type->arg_types, debug_metadata,
                           fn_type->variadic_type);

    if (hint_return_type) {
      type_resolver_.Resolve(fn_type->return_type, hint_return_type->type_id,
                             hint_return_type->metadata);
    }

    // Even if the arguments can not be properly type checked we should
    // resolve to the return type to prevent cascading errors :^).
    return ExpressionResult{type_resolver_.Rewrite(fn_type->return_type)};
  }

  if (const auto* const struct_type =
          type_registry_.GetType<StructType>(callable_type_id)) {
    SubstitutionMap substitution_map;
    PopulateInstanceTypes(*struct_type, substitution_map);

    std::vector<TypeId> realized_field_types;
    realized_field_types.reserve(struct_type->symbol->field_types.size());

    // Field types are generic and must be realized against `struct_type`'s
    // template arguments and the current inference environment
    std::ranges::transform(
        struct_type->symbol->field_types,
        std::back_inserter(realized_field_types), [&](TypeId field_type) {
          // i.e. generic Array[T] => instance Array[$0] => inferred Array[i32]
          return type_resolver_.Rewrite(
              type_rewriter_.Rewrite(field_type, substitution_map));
        });

    TypeCheckCallArguments(argument_results, realized_field_types,
                           debug_metadata,
                           /*variadic_type=*/std::nullopt);
    CHECK(!call_expr.resolved) << "CallExpression was previously resolved";
    call_expr.resolved =
        ResolvedCall{callee_result.binding->GetSymbolId(),
                     struct_type->symbol->interface_types.empty()
                         ? FunctionKind::Constructor
                         : FunctionKind::ConstructorVirtual};

    if (hint_return_type) {
      type_resolver_.Resolve(callable_type_id, hint_return_type->type_id,
                             hint_return_type->metadata);
    }
    return ExpressionResult{type_resolver_.Rewrite(callable_type_id)};
  }

  if (const auto* const alias_type =
          type_registry_.GetType<AliasType>(callable_type_id)) {
    return TypeCheckCallExpr(call_expr,
                             ExpressionResult{alias_type->target_type_id},
                             hint_return_type, debug_metadata);
  }

  error_collector_.Add("type is not callable: " +
                           type_registry_.GetNameFromTypeId(callable_type_id),
                       debug_metadata);
  return std::nullopt;
}

std::optional<ExpressionResult> ExpressionChecker::HandlePrimary(
    PrimaryExpression& primary_expression,
    Metadata metadata) {
  return std::visit(
      Overloaded{
          [&](const StringLiteral&) -> std::optional<ExpressionResult> {
            return ExpressionResult(LiteralType::String);
          },
          [&](Identifier& ident) -> std::optional<ExpressionResult> {
            if (ident.name == "Nil")
              return ExpressionResult(LiteralType::Nil);

            auto instantiate_result = [&](const NamedBinding& binding) {
              if (binding.symbol_id) {
                SubstitutionMap substitution_map;
                BuildPlaceholderTypes(binding.GetSymbolId(), substitution_map);

                TypeId realized_type_id = type_resolver_.Rewrite(
                    type_rewriter_.Rewrite(binding.type_id, substitution_map));

                return ExpressionResult::with_type_override(realized_type_id,
                                                            binding);
              }

              return ExpressionResult::of_binding(binding);
            };

            // Search within the current function scope for value.
            auto binding = scope_manager_.FindBindingFor(
                ident.name, ScopeManager::Function);
            if (binding) {
              CHECK(!ident.resolved) << "Identifier was previously resolved";
              ident.resolved = ResolvedIdentifier{*binding};
              if (narrowed_bindings_.contains(binding->binding_id)) {
                return ExpressionResult::with_storage_override(
                    narrowed_bindings_.at(binding->binding_id),
                    binding->type_id, binding);
              }
              return instantiate_result(*binding);
            }

            // Fallback search to the parent function scope.
            binding = scope_manager_.FindBindingFor(ident.name,
                                                    ScopeManager::Closure);
            if (binding) {
              // Any value symbols found now must be captured.
              if (binding->kind == NamedBinding::Variable ||
                  binding->kind == NamedBinding::Capture) {
                required_captures_.push_back(*binding);
                // Variables will ALWAYS have a realized TypeId.
                binding = scope_manager_.DeclareCaptureBinding(
                    binding->name, binding->type_id);
              }

              CHECK(!ident.resolved) << "Identifier was previously resolved";
              ident.resolved = ResolvedIdentifier{*binding};
              if (narrowed_bindings_.contains(binding->binding_id)) {
                return ExpressionResult::with_storage_override(
                    narrowed_bindings_.at(binding->binding_id),
                    binding->type_id, binding);
              }
              return instantiate_result(*binding);
            }

            // Fallback search to ALL scopes for top-level  declarations i.e.
            // functions, structs, interfaces, alias.
            binding =
                scope_manager_.FindBindingFor(ident.name, ScopeManager::All);
            if (binding) {
              CHECK(!ident.resolved) << "Identifier was previously resolved";

              switch (binding->kind) {
                case NamedBinding::Function:
                case NamedBinding::Struct:
                  ident.resolved = ResolvedIdentifier{*binding};
                  return instantiate_result(*binding);

                case NamedBinding::TypeAlias:
                case NamedBinding::Template:
                case NamedBinding::Interface:
                  ident.resolved = ResolvedIdentifier{*binding};
                  return ExpressionResult::of_binding(*binding);

                case NamedBinding::Argument:
                case NamedBinding::Capture:
                case NamedBinding::Field:
                case NamedBinding::Variable:
                  error_collector_.Add("refers to variable out of scope",
                                       metadata);
                  return std::nullopt;
              }
            }

            error_collector_.Add("unknown identifier: '" + ident.name + "'",
                                 metadata);
            return std::nullopt;
          },
          [&](int32_t) -> std::optional<ExpressionResult> {
            return ExpressionResult(LiteralType::i32);
          },
          [&](const CodepointLiteral&) -> std::optional<ExpressionResult> {
            return ExpressionResult(LiteralType::Codepoint);
          },
          [&](float) -> std::optional<ExpressionResult> {
            return ExpressionResult(LiteralType::f32);
          },
          [&](bool) -> std::optional<ExpressionResult> {
            return ExpressionResult(LiteralType::Bool);
          },
          [&](Nil) -> std::optional<ExpressionResult> {
            return ExpressionResult(LiteralType::Nil);
          }},
      primary_expression.value);
}

std::optional<ExpressionResult> ExpressionChecker::HandleMemberAccess(
    MemberAccessExpression& member_access) {
  auto object_result = Check(member_access.object);
  if (!object_result)
    return std::nullopt;

  const auto& member_name = member_access.member_name;

  if (object_result->is_value()) {
    TypeId type_id = object_result->type_id;

    // Handles member access for constrained template variables i.e T: Hashable
    if (auto* template_variable_type =
            type_registry_.GetType<TemplateVariableType>(type_id)) {
      if (!template_variable_type->constraint_type_id) {
        error_collector_
            .Add("no member '" + member_name.text +
                     "' found on unconstrained template variable '" +
                     template_variable_type->name.text + "'",
                 member_name.metadata)
            .WithNote("defined here", template_variable_type->name.metadata);
        return std::nullopt;
      }

      type_id = template_variable_type->constraint_type_id->type_id;
    }

    // Handles member access for structs wrapped in optional i.e. T?
    if (auto* optional_type = type_registry_.GetType<OptionalType>(type_id)) {
      error_collector_.Add("optional type " +
                               type_registry_.GetNameFromTypeId(type_id) +
                               " must be unwrapped",
                           member_access.object->meta);

      // Proceed with the unwrapped type to collect any further errors
      type_id = optional_type->wrapped_type;
    }

    const auto* const struct_type = type_registry_.GetType<StructType>(type_id);
    if (!struct_type) {
      error_collector_.Add("type " + type_registry_.GetNameFromTypeId(type_id) +
                               " does not support member access",
                           member_access.object->meta);
      return std::nullopt;
    }

    if (auto binding = scope_manager_.FindBindingFor(
            member_name.text, ScopeManager::Current,
            struct_type->symbol->instance_scope_id)) {
      SubstitutionMap substitution_map;
      PopulateInstanceTypes(*struct_type, substitution_map);

      if (binding->kind == NamedBinding::Field) {
        CHECK(binding->idx)
            << "member symbol must have an index for member access";
        CHECK(!member_access.resolved)
            << "MemberAccessExpression was previously resolved";
        member_access.resolved =
            ResolvedAccess{ResolvedAccess::Field{binding->idx.value()}};
      } else if (binding->kind == NamedBinding::Function) {
        CHECK(!member_access.resolved)
            << "MemberAccessExpression was previously resolved";
        member_access.resolved =
            ResolvedAccess{ResolvedAccess::Method{binding->GetSymbolId()}};
        // Ensures templated methods are instantiated with placeholders.
        BuildPlaceholderTypes(binding->GetSymbolId(), substitution_map);
      }

      TypeId realized_type_id = type_resolver_.Rewrite(
          type_rewriter_.Rewrite(binding->type_id, substitution_map));

      return ExpressionResult::with_type_override(realized_type_id, *binding);
    }

    for (ScopeId interface_scope_id : struct_type->symbol->interface_scopes) {
      if (auto binding = scope_manager_.FindBindingFor(
              member_name.text, ScopeManager::Current, interface_scope_id)) {
        CHECK_EQ(binding->kind, NamedBinding::Function)
            << "Interfaces must only contain functions";
        CHECK(!member_access.resolved)
            << "MemberAccessExpression was previously resolved";
        member_access.resolved =
            ResolvedAccess{ResolvedAccess::Method{binding->GetSymbolId()}};

        SubstitutionMap substitution_map;
        PopulateInstanceTypes(*struct_type, substitution_map);
        BuildPlaceholderTypes(binding->GetSymbolId(), substitution_map);

        TypeId realized_type_id = type_resolver_.Rewrite(
            type_rewriter_.Rewrite(binding->type_id, substitution_map));

        return ExpressionResult::with_type_override(realized_type_id, *binding);
      }
    }

    error_collector_
        .Add("no member '" + member_name.text + "' found on instance of " +
                 type_registry_.GetNameFromTypeId(type_id),
             member_name.metadata)
        .WithNote("declared here",
                  struct_type->symbol->declaration.name.metadata);
    return std::nullopt;
  }

  if (object_result->is_type_ref() && object_result->binding &&
      object_result->binding->symbol_id) {
    if (object_result->binding->kind != NamedBinding::Struct) {
      std::stringstream ss;
      ss << "binding of kind '" << object_result->binding->kind
         << "' does not support member access";
      error_collector_.Add(ss.str(), member_access.object->meta);
      return std::nullopt;
    }

    const auto& struct_symbol = type_registry_.GetSymbolChecked<StructSymbol>(
        object_result->binding->GetSymbolId());

    if (auto binding = scope_manager_.FindBindingFor(
            member_name.text, ScopeManager::Current,
            struct_symbol.static_scope_id)) {
      // Filters out non-static methods (those that require `self` with kind
      // `Method`) and fields which are only accessible on an instance.
      if (binding->kind != NamedBinding::Function) {
        std::stringstream ss;
        ss << "binding of type '" << binding->kind
           << "' can only be accessed on an instance of "
           << struct_symbol.declaration.name.text;
        error_collector_.Add(ss.str(), member_access.member_name.metadata)
            .WithNote("declared here", binding->name.metadata);
        return std::nullopt;
      }

      CHECK(!member_access.resolved)
          << "MemberAccessExpression was previously resolved";
      member_access.resolved =
          ResolvedAccess{ResolvedAccess::Function{binding->GetSymbolId()}};

      SubstitutionMap substitution_map;
      BuildPlaceholderTypes(binding->GetSymbolId(), substitution_map);

      TypeId realized_type_id = type_resolver_.Rewrite(
          type_rewriter_.Rewrite(binding->type_id, substitution_map));

      return ExpressionResult::with_type_override(realized_type_id, *binding);
    }

    error_collector_
        .Add("no member '" + member_name.text + "' found on type '" +
                 struct_symbol.declaration.name.text + "'",
             member_name.metadata)
        .WithNote("declared here", struct_symbol.declaration.name.metadata);
    return std::nullopt;
  }

  NOTREACHED() << "unhandled condition in MemberAccessExpression";
  return std::nullopt;
}

std::optional<ExpressionResult> ExpressionChecker::RequireValue(
    std::unique_ptr<Expression>& expression,
    std::optional<SpannedType> hint_expected_type) {
  auto result = Check(expression, hint_expected_type);

  if (!result)
    return std::nullopt;

  // Ensure it is an instance i.e. 123, x, fn foo().
  if (!result->is_value()) {
    std::string type_name = type_registry_.GetNameFromTypeId(result->type_id);

    error_collector_.Add("expected value, but found " + type_name,
                         expression->meta);
    return std::nullopt;
  }

  return result;
}

void ExpressionChecker::BuildPlaceholderTypes(
    SymbolId symbol_id,
    SubstitutionMap& substitution_map) {
  const auto local_template_variables = [&]() -> std::span<TypeId> {
    if (auto* symbol = type_registry_.GetSymbol<FunctionSymbol>(symbol_id))
      return symbol->template_variable_type_ids;
    if (auto* symbol = type_registry_.GetSymbol<StructSymbol>(symbol_id))
      return symbol->template_variable_type_ids;
    NOTREACHED() << "Template variables only exist on Function/Struct";
    return {};
  }();

  for (TypeId template_type_id : local_template_variables) {
    // Insert will not overwrite any previous values. There should be no overlap
    // between an outer struct's template variable TypeIds and an inner
    // function's as they are globally unique.
    auto [it, success] = substitution_map.insert(
        {template_type_id, type_resolver_.NewPlaceholder(template_type_id)});
    CHECK(success) << type_registry_.GetNameFromTypeId(template_type_id)
                   << " was previously mapped";
  }
}

std::ostream& operator<<(std::ostream& os,
                         const std::optional<ExpressionResult>& result) {
  if (!result.has_value())
    return os << "_";

  os << "{ kind: ";
  if (result->is_value()) {
    os << "Value";
  } else if (result->is_type_ref()) {
    os << "TypeRef";
  }
  os << ", type_id: " << result->type_id << ", binding: ";
  if (result->binding) {
    os << *result->binding;
  } else {
    os << "_";
  }
  return os << " }";
}
