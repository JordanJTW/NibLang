// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <optional>
#include <unordered_map>
#include <vector>

#include "compiler/error_collector.h"
#include "compiler/parser/tokenizer.h"
#include "compiler/scope_manager.h"
#include "compiler/type_context.h"
#include "compiler/type_registry.h"
#include "compiler/types.h"

using NarrowedBindings = std::unordered_map<BindingId, TypeId>;

struct ExpressionResult {
  explicit ExpressionResult(TypeId type_id) : type_id(type_id) {}

  static ExpressionResult of_binding(NamedBinding binding) {
    ExpressionResult res;
    res.type_id = binding.realized_type_id;
    res.binding = std::move(binding);
    return res;
  }

  static ExpressionResult with_type_override(
      TypeId type_id,
      std::optional<NamedBinding> binding) {
    if (!binding)
      return ExpressionResult(type_id);

    ExpressionResult result = of_binding(std::move(*binding));
    result.type_id = type_id;
    return result;
  }

  // Is this an instance? i.e. "hello world", 2 + 2, foo
  // If no binding is given then assume a temporary value with only a TypeId.
  bool is_value() const { return binding ? binding->IsValue() : true; }
  // Is this a type? i.e. String, i32, Point. Types MUST have a binding.
  bool is_type_ref() const { return binding ? binding->IsTypeRef() : false; }
  bool has_type_id() const { return type_id.has_value(); }

  std::optional<TypeId> type_id;
  std::optional<NamedBinding> binding;

  NarrowedBindings true_bindings;
  NarrowedBindings false_bindings;

 private:
  ExpressionResult() = default;
};

class ExpressionChecker {
 public:
  explicit ExpressionChecker(ScopeManager& scope_manager,
                             TypeContext& type_context,
                             TypeRegistry& type_registry,
                             NarrowedBindings narrowed_bindings,
                             std::vector<NamedBinding>& required_captures,
                             ErrorCollector& error_collector);

  std::optional<ExpressionResult> RequireConcreteValue(
      std::unique_ptr<Expression>& expression,
      std::optional<SpannedType> hint_return_type = std::nullopt);

 private:
  std::optional<ExpressionResult> HandlePrimary(PrimaryExpression&, Metadata);
  std::optional<ExpressionResult> HandleMemberAccess(MemberAccessExpression&);

  struct ArgumentResult {
    std::optional<ExpressionResult> result;
    std::optional<Metadata> metadata;
  };

  void TypeCheckCallArguments(
      const std::vector<std::optional<SpannedType>>& call_argument_results,
      const std::vector<TypeId>& expected_argument_types,
      const Metadata& debug_metadata,
      std::optional<TypeId> variadic_type);

  std::optional<ExpressionResult> TypeCheckCallExpr(
      CallExpression& call_expr,
      ExpressionResult callee_result,
      std::optional<SpannedType> hint_return_type,
      Metadata debug_metadata);

  std::optional<ExpressionResult> Check(
      std::unique_ptr<Expression>& expression,
      std::optional<SpannedType> hint_return_type = std::nullopt);

  ScopeManager& scope_manager_;
  TypeContext& type_context_;
  TypeRegistry& type_registry_;
  const NarrowedBindings narrowed_bindings_;
  std::vector<NamedBinding>& required_captures_;
  ErrorCollector& error_collector_;
};

std::ostream& operator<<(std::ostream& os,
                         const std::optional<ExpressionResult>& result);