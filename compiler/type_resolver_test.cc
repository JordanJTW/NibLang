// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/type_resolver.h"

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

inline ParsedType MakeOptional(ParsedType wrapped) {
  return {ParsedOptionalType{std::make_shared<ParsedType>(std::move(wrapped))}};
}

inline ParsedType MakeParameterized(std::string base,
                                    std::vector<ParsedType> params) {
  return {ParsedParameterizedType{
      std::make_shared<ParsedType>(ParsedType{base}), std::move(params)}};
}

inline ParsedType MakeFunction(std::vector<ParsedType> args, ParsedType ret) {
  return {ParsedFunctionType{std::move(args),
                             std::make_shared<ParsedType>(std::move(ret))}};
}

MATCHER_P(IsType, expected_name, "") {
  if (!std::holds_alternative<std::string>(arg.type))
    return false;
  return std::get<std::string>(arg.type) == expected_name;
}

class TypeResolverTest : public ::testing::Test {
 protected:
  ErrorCollector error_collector;
  ScopeManager scope_manager;
  TypeContext type_context{scope_manager};
  TypeResolver type_resolver{type_context, error_collector};

  TypeResolver::Bindings bindings;
};

TEST_F(TypeResolverTest, BindToPrimaryType) {
  // Pattern: T, Concrete: i32
  auto pattern = ParsedType{"T"};
  auto concrete = ParsedType{"i32"};

  EXPECT_TRUE(type_resolver.Resolve(pattern, concrete, {"T"}, bindings));
  EXPECT_EQ(bindings.size(), 1u);
  EXPECT_THAT(bindings["T"], IsType("i32"));
}

TEST_F(TypeResolverTest, ResolveConcreteTypes) {
  // Pattern: Array<i32>, Concrete: Array<f64>
  auto pattern = MakeParameterized("Array", {{"i32"}});
  auto concrete = MakeParameterized("Array", {{"f64"}});

  EXPECT_FALSE(type_resolver.Resolve(pattern, concrete, {}, bindings));
}

TEST_F(TypeResolverTest, BindNestedTemplateParameters) {
  // Pattern: Map<K, V>, Concrete: Map<string, i32>
  auto pattern = MakeParameterized("Map", {{"K"}, {"V"}});
  auto concrete = MakeParameterized("Map", {{"string"}, {"i32"}});

  EXPECT_TRUE(type_resolver.Resolve(pattern, concrete, {"K", "V"}, bindings));
  EXPECT_THAT(bindings["K"], IsType("string"));
  EXPECT_THAT(bindings["V"], IsType("i32"));
}

TEST_F(TypeResolverTest, ConsistencyCheckMultiAppearance) {
  // Pattern: (T, T) -> bool, Concrete: (i32, i32) -> bool
  auto pattern = MakeFunction({{"T"}, {"T"}}, {"bool"});
  auto concrete = MakeFunction({{"i32"}, {"i32"}}, {"bool"});

  EXPECT_TRUE(type_resolver.Resolve(pattern, concrete, {"T"}, bindings));
  EXPECT_THAT(bindings["T"], IsType("i32"));
}

TEST_F(TypeResolverTest, FailsOnInconsistentBindings) {
  // Pattern: (T, T) -> bool, Concrete: (i32, f64) -> bool
  auto pattern = MakeFunction({{"T"}, {"T"}}, {"bool"});
  auto concrete = MakeFunction({{"i32"}, {"f64"}}, {"bool"});

  EXPECT_FALSE(type_resolver.Resolve(pattern, concrete, {"T"}, bindings));
}

TEST_F(TypeResolverTest, ComplexDeepMatching) {
  // Pattern: Optional<T>, Concrete: Optional<Vector<f32>>
  auto pattern = MakeOptional({"T"});
  auto inner_concrete = MakeParameterized("Array", {{"f32"}});
  auto concrete = MakeOptional(inner_concrete);

  EXPECT_TRUE(type_resolver.Resolve(pattern, concrete, {"T"}, bindings));

  ASSERT_TRUE(
      std::holds_alternative<ParsedParameterizedType>(bindings["T"].type));
  auto& p = std::get<ParsedParameterizedType>(bindings["T"].type);
  EXPECT_THAT(*p.type, IsType("Array"));
}

TEST_F(TypeResolverTest, FunctionReturnMatch) {
  // Pattern: fn (i32)->T, Concrete: fn (i32)->Array[f32];
  auto pattern = MakeFunction({{"i32"}}, {"T"});
  auto concrete =
      MakeFunction({{"i32"}}, MakeParameterized("Array", {{"f32"}}));

  EXPECT_TRUE(type_resolver.Resolve(pattern, concrete, {"T"}, bindings));

  ASSERT_TRUE(
      std::holds_alternative<ParsedParameterizedType>(bindings["T"].type));
  auto& p = std::get<ParsedParameterizedType>(bindings["T"].type);
  EXPECT_THAT(*p.type, IsType("Array"));
}

TEST_F(TypeResolverTest, FunctionWithTemplateArgAndReturn) {
  // Pattern: fn (T)->RT, Concrete: fn (i32)->String;
  auto pattern = MakeFunction({{"T"}}, {"RT"});
  auto concrete = MakeFunction({{"i32"}}, {"String"});

  EXPECT_TRUE(type_resolver.Resolve(pattern, concrete, {"T", "RT"}, bindings));
  EXPECT_THAT(bindings["T"], IsType("i32"));
  EXPECT_THAT(bindings["RT"], IsType("String"));
}

TEST_F(TypeResolverTest, OptionalWithNil) {
  // Pattern: T?, Concrete: Nil;
  auto pattern = MakeOptional({"T"});
  auto concrete = ParsedType{"Nil"};

  EXPECT_TRUE(type_resolver.Resolve(pattern, concrete, {"T"}, bindings));
  EXPECT_THAT(bindings["T"], IsType("Nil"));
}

// It is valid to make the type more specific i.e. narrowed from Optional.
// This allows for optional parameters to functions.
TEST_F(TypeResolverTest, OptionalWithNonOptionalType) {
  // Pattern: T?, Concrete: String;
  auto pattern = MakeOptional({"T"});
  auto concrete = ParsedType{"String"};

  EXPECT_TRUE(type_resolver.Resolve(pattern, concrete, {"T"}, bindings));
  EXPECT_THAT(bindings["T"], IsType("String"));
}

}  // namespace