// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "compiler/error_collector.h"
#include "compiler/tokenizer.h"
#include "compiler/type_context.h"
#include "compiler/types.h"

class SemanticAnalyzer {
 public:
  explicit SemanticAnalyzer(TypeContext& type_context,
                            ScopeManager& scope_manager,
                            ErrorCollector& error_collector,
                            TypeRegistry& type_registry);

  struct FunctionContext {
    std::vector<NamedBinding> required_captures;
    const TypeId return_type_id;
  };

  void Check(Block& block, FunctionContext& context);

  struct ScopeNarrowingInfo {
    NamedBinding symbol;
    TypeId if_branch_type;
    TypeId else_branch_type;
  };


  struct ExpressionResult {
    explicit ExpressionResult(TypeId type_id)
        : type_id(type_id) {}

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

      ExpressionResult result = ExpressionResult::of_binding(*binding);
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
    bool should_curry_method_self = false;
    std::vector<ScopeNarrowingInfo> narrowing_info = {};

   private:
    ExpressionResult() = default;
  };

  using Result = std::optional<SemanticAnalyzer::ExpressionResult>;

  std::optional<ExpressionResult> CheckExpression(
      std::unique_ptr<Expression>& expression,
      FunctionContext& context);

 private:
  void CheckStatement(std::unique_ptr<Statement>& statement,
                      FunctionContext& context);

  Result HandleMemberAccess(MemberAccessExpression&, FunctionContext&);

  struct ArgumentResult {
    SemanticAnalyzer::Result result;
    std::optional<Metadata> metadata;
  };

  void TypeCheckCallArguments(
      const std::vector<std::optional<SpannedType>>& call_arugment_results,
      const std::vector<TypeId>& expected_argument_types,
      const Metadata& debug_metadata,
      std::optional<TypeId> variadic_type);

  Result TypeCheckCallExpr(CallExpression& call_expr,
                           ExpressionResult callee_result,
                           FunctionContext& context,
                           Metadata debug_metdata);

  Result RequireConcreteValue(std::unique_ptr<Expression>& expression,
                              FunctionContext& context);

  TypeContext& type_context_;
  ScopeManager& scope_manager_;
  ErrorCollector& error_collector_;
  TypeRegistry& type_registry_;
};

std::ostream& operator<<(std::ostream& os, SemanticAnalyzer::Result result);