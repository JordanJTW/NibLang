#pragma once

#include <map>
#include <optional>
#include <set>
#include <variant>
#include <vector>

#include "compiler/error_collector.h"
#include "compiler/type_context.h"
#include "compiler/types.h"

class SemanticAnalyzer {
 public:
  explicit SemanticAnalyzer(TypeContext& type_context,
                            ErrorCollector& error_collector);

  void Check(Block& block);

  struct ExpressionResult {
    TypeId type_id;
    std::optional<Symbol> symbol;
  };

  using Result = std::optional<SemanticAnalyzer::ExpressionResult>;

  std::optional<ExpressionResult> CheckExpression(
      std::unique_ptr<Expression>& expression);

 private:
  void CheckStatement(std::unique_ptr<Statement>& statement);
  void CheckFunctionBody(FunctionDeclaration& fn);

  Result TypeCheckCallExpr(CallExpression& call_expr,
                           TypeId callee_type_id,
                           Symbol callee_symbol,
                           Metadata debug_metdata);

  TypeContext& type_context_;
  ErrorCollector& error_collector_;
};