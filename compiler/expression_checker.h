/*
 * Copyright (C) 2026, Jordan Werthman <jordanwerthman@gmail.com>.
 *
 * This software is dual-licensed. You may use this file under the terms of
 * the GNU General Public License v3.0 (GPL-3.0) OR under a commercial
 * license obtained directly from the copyright holder.
 *
 * See the LICENSE.md file in the project root for full licensing details.
 *
 * SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-Commercial
 */

#pragma once

#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include "compiler/error_collector.h"
#include "compiler/parser/tokenizer.h"
#include "compiler/scope_manager.h"
#include "compiler/type_context.h"
#include "compiler/type_registry.h"
#include "compiler/type_resolver.h"
#include "compiler/type_rewriter.h"
#include "compiler/types.h"

using NarrowedBindings = std::unordered_map<BindingId, TypeId>;

struct ExpressionResult {
  explicit ExpressionResult(TypeId type_id) : type_id(type_id) {}

  static ExpressionResult of_binding(NamedBinding binding) {
    ExpressionResult res(binding.type_id);
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

  ExpressionResult&& with_storage_override(TypeId type_id) && {
    this->storage_type_id = type_id;
    return std::move(*this);
  }

  ExpressionResult&& with_substitutions(SubstitutionMap new_substitutions) && {
    this->substitution_map.insert(new_substitutions.begin(),
                                  new_substitutions.end());
    return std::move(*this);
  }

  // Is this an instance? i.e. "hello world", 2 + 2, foo
  // If no binding is given then assume a temporary value with only a TypeId.
  bool is_value() const { return binding ? binding->IsValue() : true; }
  // Is this a type? i.e. String, i32, Point. Types MUST have a binding.
  bool is_type_ref() const { return binding ? binding->IsTypeRef() : false; }

  TypeId type_id;
  std::optional<TypeId> storage_type_id;
  std::optional<NamedBinding> binding;
  SubstitutionMap substitution_map;

  NarrowedBindings true_bindings;
  NarrowedBindings false_bindings;
};

class ExpressionChecker {
 public:
  explicit ExpressionChecker(ScopeManager& scope_manager,
                             TypeContext& type_context,
                             TypeRegistry& type_registry,
                             NarrowedBindings narrowed_bindings,
                             std::vector<NamedBinding>& required_captures,
                             ErrorCollector& error_collector);

  std::optional<ExpressionResult> CheckChain(
      std::unique_ptr<Expression>& root_expression,
      std::vector<TypeId>& result_type_ids_for_chain,
      std::optional<SpannedType> hint_return_type = std::nullopt);

 private:
  std::optional<ExpressionResult> HandlePrimary(PrimaryExpression&, Metadata);
  std::optional<ExpressionResult> HandleMemberAccess(MemberAccessExpression&);
  std::optional<ExpressionResult> HandleBinaryEquality(
      BinaryExpression& binary_expression,
      Metadata expression_metadata);
  std::optional<ExpressionResult> HandleBinaryComparison(
      BinaryExpression& binary_expression,
      Metadata expression_metadata);
  std::optional<ExpressionResult> HandleBinaryInfix(
      BinaryExpression& binary_expression,
      Metadata expression_metadata);

  struct ArgumentResult {
    std::optional<ExpressionResult> result;
    std::optional<Metadata> metadata;
  };

  std::optional<ExpressionResult> RequireValue(
      std::unique_ptr<Expression>& expression,
      std::optional<SpannedType> hint_return_type = std::nullopt);

  void TypeCheckCallArguments(
      std::vector<std::unique_ptr<Expression>>& argument_expressions,
      const std::vector<TypeId>& expected_argument_types,
      const Metadata& call_site_metadata,
      const Metadata& callee_metadata,
      std::optional<TypeId> variadic_type);

  std::optional<ExpressionResult> TypeCheckCallExpr(
      CallExpression& call_expr,
      ExpressionResult callee_result,
      std::optional<SpannedType> hint_return_type,
      Metadata debug_metadata);

  std::optional<ExpressionResult> Check(
      std::unique_ptr<Expression>& expression,
      std::optional<SpannedType> hint_return_type = std::nullopt);

  void BuildPlaceholderTypes(SymbolId symbol_id,
                             SubstitutionMap& substitution_map);

  std::optional<NamedBinding> FindImplementationFor(
      SpannedType receiver_type,
      std::string_view interface_name,
      std::string_view method_name,
      std::string_view error_message,
      Metadata trigger_span) const;

  std::optional<ImplicitCast> CalculateImplicitCast(
      const Expression& expression,
      TypeId expected_type) const;

  ScopeManager& scope_manager_;
  TypeContext& type_context_;
  TypeRegistry& type_registry_;
  NarrowedBindings narrowed_bindings_;
  std::vector<NamedBinding>& required_captures_;
  ErrorCollector& error_collector_;

  TypeResolver type_resolver_;
  TypeRewriter type_rewriter_{type_registry_, type_context_};

  // Iteration order must be maintained
  std::vector<std::function<void()>> deferred_type_resolution_;

  std::vector<TypeId> result_type_ids_for_chain_;
};

std::ostream& operator<<(std::ostream& os,
                         const std::optional<ExpressionResult>& result);