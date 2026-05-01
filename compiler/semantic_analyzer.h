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
                            ErrorCollector& error_collector);

  void Check(Block& block);

  struct ScopeNarrowingInfo {
    Symbol symbol;
    TypeId if_branch_type;
    TypeId else_branch_type;
  };

  struct ExpressionResult {
    TypeId type_id;
    std::optional<Symbol> symbol;
    std::vector<ScopeNarrowingInfo> narrowing_info = {};
  };

  using Result = std::optional<SemanticAnalyzer::ExpressionResult>;

  std::optional<ExpressionResult> CheckExpression(
      std::unique_ptr<Expression>& expression);

 private:
  void CheckStatement(std::unique_ptr<Statement>& statement);
  void CheckFunctionBody(FunctionDeclaration& fn);

  struct ArgumentResult {
    SemanticAnalyzer::Result result;
    std::optional<Metadata> metadata;
  };

  void TypeCheckCallArguments(
      const std::vector<ArgumentResult>& call_arugment_results,
      const std::vector<TypeId>& expected_argument_types,
      const Metadata& debug_metadata,
      bool is_variadic_function = false);

  Result TypeCheckCallExpr(CallExpression& call_expr,
                           ExpressionResult callee_result,
                           Metadata debug_metdata);

  TypeContext& type_context_;
  ErrorCollector& error_collector_;
};