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

class TypeResolverTest : public ::testing::Test {
 protected:
  ErrorCollector error_collector;
  ScopeManager scope_manager{error_collector};
  TypeRegistry type_registry{scope_manager};
  TypeContext type_context{scope_manager, type_registry, error_collector};
  TypeResolver type_resolver{type_registry, type_context, error_collector};

  TypeResolver::Bindings bindings;

  TypeId NewType(const ParsedType& type) {
    auto type_id = type_context.GetTypeIdFor(type);
    CHECK(type_id.has_value()) << "TypeId must be created in test";
    return *type_id;
  }

  TypeId NewType(const ParsedType& type,
                 const std::vector<std::string>& template_variables) {
    // Allows GetTypeIdFor() to find template variables as placeholders in scope
    for (size_t idx = 0; idx < template_variables.size(); ++idx) {
      scope_manager.InsertNameIntoScope(
          SpannedText{template_variables[idx]}, NamedBinding::Template,
          type_registry.NewPlaceholderType(idx), /*symbol_id=*/std::nullopt);
    }
    return NewType(type);
  }

  void SetUp() override {
    static StructDeclaration array_declaration = {
        .name = SpannedText{"Array"},
        .template_arguments = {{"T"}},
    };
    type_registry.NewStructSymbol(array_declaration);
  }
};

TEST_F(TypeResolverTest, BindToPrimaryType) {
  // Pattern: T, Concrete: i32
  auto pattern = NewType(ParsedType{"T"}, {"T"});
  auto concrete = NewType(ParsedType{"i32"});

  EXPECT_TRUE(type_resolver.Resolve(pattern, concrete, bindings));
  EXPECT_EQ(bindings.size(), 1u);
  EXPECT_EQ(bindings[0], LiteralType::i32);
}

TEST_F(TypeResolverTest, ResolveConcreteTypes) {
  // Pattern: Array<i32>, Concrete: Array<f64>
  auto pattern = NewType(MakeParameterized("Array", {{"i32"}}));
  auto concrete = NewType(MakeParameterized("Array", {{"f32"}}));

  EXPECT_FALSE(type_resolver.Resolve(pattern, concrete, bindings));
}

TEST_F(TypeResolverTest, BindNestedTemplateParameters) {
  // Pattern: Array<Array<T>>, Concrete: Array<Array<bool>>
  auto pattern = NewType(
      MakeParameterized("Array", {MakeParameterized("Array", {{"T"}})}), {"T"});
  auto concrete = NewType(
      MakeParameterized("Array", {MakeParameterized("Array", {{"bool"}})}));

  EXPECT_TRUE(type_resolver.Resolve(pattern, concrete, bindings));
  EXPECT_EQ(bindings[0], LiteralType::Bool);
}

TEST_F(TypeResolverTest, ConsistencyCheckMultiAppearance) {
  // Pattern: (T, T) -> bool, Concrete: (i32, i32) -> bool
  auto pattern = NewType(MakeFunction({{"T"}, {"T"}}, {"bool"}), {"T"});
  auto concrete = NewType(MakeFunction({{"i32"}, {"i32"}}, {"bool"}));

  EXPECT_TRUE(type_resolver.Resolve(pattern, concrete, bindings));
  EXPECT_EQ(bindings[0], LiteralType::i32);
}

TEST_F(TypeResolverTest, FailsOnInconsistentBindings) {
  // Pattern: (T, T) -> bool, Concrete: (i32, f64) -> bool
  auto pattern = NewType(MakeFunction({{"T"}, {"T"}}, {"bool"}), {"T"});
  auto concrete = NewType(MakeFunction({{"i32"}, {"f32"}}, {"bool"}));

  EXPECT_FALSE(type_resolver.Resolve(pattern, concrete, bindings));
}

// It is valid to make the type more specific i.e. narrowed from Optional.
// This allows for optional parameters to functions.
TEST_F(TypeResolverTest, ImplicitOptionalPromotion) {
  // Pattern: Optional<T>, Concrete: Array<f32>
  auto pattern = NewType(MakeOptional({"T"}), {"T"});
  auto concrete = NewType(MakeParameterized("Array", {{"f32"}}));

  EXPECT_TRUE(type_resolver.Resolve(pattern, concrete, bindings));
  EXPECT_EQ(bindings[0], concrete);
}

TEST_F(TypeResolverTest, FunctionReturnMatch) {
  // Pattern: fn (i32)->T, Concrete: fn (i32)->Array[f32];
  auto pattern = NewType(MakeFunction({{"i32"}}, {"T"}), {"T"});
  auto return_type = MakeParameterized("Array", {{"f32"}});
  auto concrete = NewType(MakeFunction({{"i32"}}, return_type));

  EXPECT_TRUE(type_resolver.Resolve(pattern, concrete, bindings));
  EXPECT_EQ(bindings[0], NewType(return_type));
}

TEST_F(TypeResolverTest, FunctionWithTemplateArgAndReturn) {
  // Pattern: fn (T)->RT, Concrete: fn (i32)->String;
  auto pattern = NewType(MakeFunction({{"T"}}, {"RT"}), {"T", "RT"});
  auto concrete = NewType(MakeFunction({{"i32"}}, {"bool"}));

  EXPECT_TRUE(type_resolver.Resolve(pattern, concrete, bindings));
  EXPECT_EQ(bindings[0], LiteralType::i32);
  EXPECT_EQ(bindings[1], LiteralType::Bool);
}

TEST_F(TypeResolverTest, OptionalWithNil) {
  // Pattern: T?, Concrete: Nil;
  auto pattern = NewType(MakeOptional({"T"}), {"T"});
  auto concrete = LiteralType::Nil;

  EXPECT_TRUE(type_resolver.Resolve(pattern, concrete, bindings));
  EXPECT_EQ(bindings[0], LiteralType::Nil);
}

TEST_F(TypeResolverTest, ResolveConstructor) {
  // struct Foo[A, B] { a: A, b: B }
  static StructDeclaration declaration = {
      .name = SpannedText{"Foo"},
      .template_arguments = {{"A"}, {"B"}},
      .fields = {{SpannedText{"a"}, ParsedType{"A"}},
                 {SpannedText{"b"}, ParsedType{"B"}}},
  };

  NamedBinding binding = type_registry.NewStructSymbol(declaration);

  std::vector<TypeId> deduced_bindings;
  EXPECT_TRUE(type_resolver.Resolve(
      binding, {SpannedType{LiteralType::f32}, SpannedType{LiteralType::Bool}},
      deduced_bindings, /*expression_metadata=*/{}));

  EXPECT_THAT(deduced_bindings,
              ElementsAre(LiteralType::f32, LiteralType::Bool));
}

TEST_F(TypeResolverTest, ResolveConstructorWithMissingArgument) {
  // struct Foo[A] { a: A, b: A }
  static StructDeclaration declaration = {
      .name = SpannedText{"Foo"},
      .template_arguments = {{"A"}},
      .fields = {{SpannedText{"a"}, ParsedType{"A"}},
                 {SpannedText{"b"}, ParsedType{"A"}}},
  };

  NamedBinding binding = type_registry.NewStructSymbol(declaration);

  std::vector<TypeId> deduced_bindings;
  EXPECT_TRUE(type_resolver.Resolve(
      binding, {std::nullopt, SpannedType{LiteralType::f32}}, deduced_bindings,
      /*expression_metadata=*/{}));

  EXPECT_THAT(deduced_bindings, ElementsAre(LiteralType::f32));

  deduced_bindings.clear();  // Ensure we get fresh bindings :^)
  EXPECT_TRUE(type_resolver.Resolve(binding, {SpannedType{LiteralType::Bool}},
                                    deduced_bindings,
                                    /*expression_metadata=*/{}));

  EXPECT_THAT(deduced_bindings, ElementsAre(LiteralType::Bool));
}

}  // namespace