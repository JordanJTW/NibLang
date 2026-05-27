// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/scope_manager.h"

#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>

#include "compiler/error_collector.h"
#include "compiler/gtest_helpers.h"
#include "compiler/type_context.h"
#include "compiler/types.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::i32;
using ::testing::ident;
using ::testing::Return;

namespace {

class ScopeManagerTest : public ::testing::Test {
 protected:
  ScopeManager scope_manager;
};

TEST_F(ScopeManagerTest, DeclareAndFindRootVariable) {
  scope_manager.DeclareVariableBinding("foo", TypeId{36});

  auto binding = scope_manager.FindBindingFor("foo", ScopeManager::Current);
  ASSERT_TRUE(binding.has_value());

  EXPECT_EQ(binding->realized_type_id, TypeId{36});
}

TEST_F(ScopeManagerTest, CorrectBindingKinds) {
  auto var_b = scope_manager.DeclareVariableBinding("v", TypeId{1});
  EXPECT_EQ(var_b.kind, NamedBinding::Variable);

  auto cap_b = scope_manager.DeclareCaptureBinding("c", TypeId{2});
  EXPECT_EQ(cap_b.kind, NamedBinding::Capture);

  auto tpl_b = scope_manager.DeclareTemplateBinding("t", TypeId{3});
  EXPECT_EQ(tpl_b.kind, NamedBinding::Template);
}

TEST_F(ScopeManagerTest, ClimbScopeTreeToFindSymbol) {
  scope_manager.DeclareVariableBinding("global_var", TypeId{100});

  scope_manager.EnterScope(ScopeManager::FunctionScope, "my_func");
  scope_manager.EnterScope(ScopeManager::BlockScope, "local_block");

  auto binding = scope_manager.FindBindingFor("global_var", ScopeManager::All);
  ASSERT_TRUE(binding.has_value());
  EXPECT_EQ(binding->kind, NamedBinding::Variable);
}

TEST_F(ScopeManagerTest, LocalShadowingHidesParent) {
  scope_manager.DeclareVariableBinding("x", TypeId{1});  // Global X

  scope_manager.EnterScope(ScopeManager::BlockScope, "inner_block");
  scope_manager.DeclareVariableBinding("x", TypeId{2});  // Inner Shadow X

  auto binding = scope_manager.FindBindingFor("x", ScopeManager::All);
  ASSERT_TRUE(binding.has_value());
  EXPECT_EQ(binding->realized_type_id, TypeId{2});
}

TEST_F(ScopeManagerTest, ScopeToCheckCurrentStrictness) {
  scope_manager.DeclareVariableBinding("outer_var", TypeId{1});

  scope_manager.EnterScope(ScopeManager::BlockScope, "inner_block");
  scope_manager.DeclareVariableBinding("inner_var", TypeId{2});

  EXPECT_TRUE(scope_manager.FindBindingFor("inner_var", ScopeManager::Current)
                  .has_value());

  EXPECT_FALSE(scope_manager.FindBindingFor("outer_var", ScopeManager::Current)
                   .has_value());
}

TEST_F(ScopeManagerTest, ScopeToCheckFunctionBoundary) {
  scope_manager.DeclareVariableBinding("global_type", TypeId{42});

  scope_manager.EnterScope(ScopeManager::FunctionScope, "foo");
  scope_manager.DeclareVariableBinding("func_local", TypeId{1});

  scope_manager.EnterScope(ScopeManager::BlockScope, "nested_block");

  EXPECT_TRUE(scope_manager.FindBindingFor("func_local", ScopeManager::Function)
                  .has_value());

  EXPECT_FALSE(
      scope_manager.FindBindingFor("global_type", ScopeManager::Function)
          .has_value());
}

TEST_F(ScopeManagerTest, NonLinearLookupViaOverrideScopeId) {
  auto global_binding =
      scope_manager.DeclareVariableBinding("module_a_const", TypeId{99});

  ScopeId branch_id =
      scope_manager.EnterScope(ScopeManager::FunctionScope, "module_b_func");
  scope_manager.DeclareVariableBinding("local_b_var", TypeId{2});

  EXPECT_TRUE(scope_manager.FindBindingFor("local_b_var", ScopeManager::All)
                  .has_value());

  auto override_lookup = scope_manager.FindBindingFor(
      "module_a_const", ScopeManager::All, /*override_scope_id=*/0);
  ASSERT_TRUE(override_lookup.has_value());
  EXPECT_EQ(override_lookup->realized_type_id, TypeId{99});

  auto isolation_lookup = scope_manager.FindBindingFor(
      "local_b_var", ScopeManager::Current, /*override_scope_id=*/0);
  EXPECT_FALSE(isolation_lookup.has_value());
}

}  // namespace