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
    ExpressionResult(TypeId id) : type_id(id) {}
    ExpressionResult(NamedBinding binding)
        : type_id(binding.realized_type_id), binding(std::move(binding)) {}
    ExpressionResult(TypeId id, std::optional<NamedBinding> binding)
        : type_id(id), binding(std::move(binding)) {}

    bool has_type_id() const { return type_id.has_value(); }

    std::optional<TypeId> type_id;
    std::optional<NamedBinding> binding;
    std::vector<ScopeNarrowingInfo> narrowing_info = {};
  };

  using Result = std::optional<SemanticAnalyzer::ExpressionResult>;

  std::optional<ExpressionResult> CheckExpression(
      std::unique_ptr<Expression>& expression,
      FunctionContext& context);

 private:
  void CheckStatement(std::unique_ptr<Statement>& statement,
                      FunctionContext& context);

  struct ArgumentResult {
    SemanticAnalyzer::Result result;
    std::optional<Metadata> metadata;
  };

  void TypeCheckCallArguments(
      const std::vector<ArgumentResult>& call_arugment_results,
      const std::vector<TypeId>& expected_argument_types,
      const Metadata& debug_metadata,
      bool is_variadic_function = false);

  std::optional<TypeId> InstantiateType(
      const std::vector<std::pair<std::string, ParsedType>>& parsed_types,
      const std::vector<ArgumentResult>& arugment_results,
      const std::vector<TemplateArgument>& template_arguments,
      const std::vector<TemplateArgument>& self_template_arguments,
      NamedBinding binding,
      std::string_view symbol_name,
      std::unordered_map<std::string, TypeId> default_template_type_ids);

  Result TypeCheckCallExpr(CallExpression& call_expr,
                           ExpressionResult callee_result,
                           FunctionContext& context,
                           Metadata debug_metdata);

  TypeContext& type_context_;
  ScopeManager& scope_manager_;
  ErrorCollector& error_collector_;
  TypeRegistry& type_registry_;
};