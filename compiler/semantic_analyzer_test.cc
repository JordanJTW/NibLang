// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/semantic_analyzer.h"

#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>

#include "compiler/error_collector.h"
#include "compiler/gtest_helpers.h"
#include "compiler/scope_manager.h"
#include "compiler/type_context.h"
#include "compiler/type_registry.h"
#include "compiler/types.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::i32;
using ::testing::ident;
using ::testing::Return;

namespace {

using LiteralType = TypeRegistry::LiteralType;

class SemanticAnalyzerTest : public ::testing::Test {
 protected:
  ErrorCollector error_collector;
  ScopeManager scope_manager{error_collector};
  TypeRegistry type_registry{scope_manager};
  TypeContext type_context{scope_manager, type_registry, error_collector};
  SemanticAnalyzer semantic_analyzer{type_context, scope_manager,
                                     error_collector, type_registry};
};

TEST_F(SemanticAnalyzerTest, Expression_PrimaryExpr_BuiltInValue) {
  static const std::vector<std::tuple<std::string, PrimaryExpression, TypeId>>
      kPrimaryExpressionToTypeId = {
          {"i32", PrimaryExpression{109}, LiteralType::i32},
          {"f32", PrimaryExpression{3.14159f}, LiteralType::f32},
          {"bool", PrimaryExpression{true}, LiteralType::Bool},
      };

  for (const auto& [name, primary_expr, expected_type_id] :
       kPrimaryExpressionToTypeId) {
    SCOPED_TRACE("Testing PrimaryExpression for: " + name);
    auto expr = std::make_unique<Expression>(Expression{primary_expr});
    SemanticAnalyzer::FunctionContext context = {{}, LiteralType::Void};
    auto result = semantic_analyzer.CheckExpression(expr, context);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type_id, expected_type_id);
    EXPECT_FALSE(result->binding.has_value());
  }
}

}  // namespace