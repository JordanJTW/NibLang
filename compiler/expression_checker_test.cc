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

#include "compiler/expression_checker.h"

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
  NarrowedBindings bindings;
  std::vector<NamedBinding> required_captures;
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
    ExpressionChecker expression_checker{scope_manager,     type_context,
                                         type_registry,     bindings,
                                         required_captures, error_collector};
    std::vector<TypeId> result_type_ids;
    auto result = expression_checker.CheckChain(expr, result_type_ids);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type_id, expected_type_id);
    EXPECT_FALSE(result->binding.has_value());
  }
}

}  // namespace