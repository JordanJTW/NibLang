#include "compiler/semantic_analyzer.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "compiler/gtest_helpers.h"
#include "compiler/parser.h"
#include "compiler/tokenizer.h"
#include "compiler/types.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::i32;
using ::testing::ident;
using ::testing::Return;

namespace {

class SemanticAnalyzerTest : public ::testing::Test {
 protected:
  std::unique_ptr<ErrorCollector> error_collector{
      DefaultErrorCollector("test_file")};
  TypeContext type_context{*error_collector};
  SemanticAnalyzer semantic_analyzer{type_context, *error_collector};
};

TEST_F(SemanticAnalyzerTest, Expression_PrimaryExpr_BuiltInValue) {
  static const std::vector<std::tuple<std::string, PrimaryExpression, TypeId>>
      kPrimaryExpressionToTypeId = {
          {"i32", PrimaryExpression{109}, TypeContext::i32},
          {"f32", PrimaryExpression{3.14159f}, TypeContext::f32},
          {"bool", PrimaryExpression{true}, TypeContext::Bool},
      };

  for (const auto& [name, primary_expr, expected_type_id] :
       kPrimaryExpressionToTypeId) {
    SCOPED_TRACE("Testing PrimaryExpression for: " + name);
    auto expr = std::make_unique<Expression>(Expression{primary_expr});
    auto result = semantic_analyzer.CheckExpression(expr);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type_id, expected_type_id);
    EXPECT_FALSE(result->symbol.has_value());
  }
}

}  // namespace