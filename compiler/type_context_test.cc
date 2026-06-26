// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/type_context.h"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "compiler/error_collector.h"
#include "compiler/type_registry.h"
#include "compiler/types.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "src/vm.h"

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::Return;

namespace {

using LiteralType = TypeRegistry::LiteralType;

class TypeContextTest : public ::testing::Test {
 protected:
  ErrorCollector error_collector;
  ScopeManager scope_manager;
  TypeRegistry type_registry{scope_manager};
  TypeContext type_context{scope_manager, type_registry, error_collector};
};

TEST_F(TypeContextTest, GetTypeIdFor_BuiltInType) {
  static const std::unordered_map<std::string, TypeId> kBuiltInTypes = {
      {"Void", LiteralType::Void}, {"i32", LiteralType::i32},
      {"f32", LiteralType::f32},   {"bool", LiteralType::Bool},
      {"any", LiteralType::Any},
  };
  for (const auto& [name, expected] : kBuiltInTypes) {
    SCOPED_TRACE("Testing built-in: " + name);
    auto type_id = type_context.GetTypeIdFor(ParsedType{name});
    ASSERT_TRUE(type_id.has_value());
    EXPECT_EQ(type_id.value(), expected);
  }
}

TEST_F(TypeContextTest, GetTypeIdFor_Function) {
  FunctionDeclaration fn_decl{
      .name = "test_fn",
      .arguments = {{"arg1", ParsedType{"i32"}}, {"arg2", ParsedType{"f32"}}},
      .return_type = ParsedType{"bool"},
      .function_kind = FunctionKind::Free,
      .body = std::make_unique<Block>(),  // non-extern MUST have body
  };

  auto symbol_id = type_registry.NewFunctionSymbol(fn_decl);
  auto symbol = type_context.DefineFunction(symbol_id);
  ASSERT_TRUE(symbol.has_value());
  EXPECT_EQ(symbol->kind, NamedBinding::Function);

  // Check that the function type is registered
  auto fn_type = type_registry.GetType<FunctionType>(*symbol->realized_type_id);
  EXPECT_EQ(fn_type->arg_types.size(), 2);
  EXPECT_EQ(fn_type->arg_types[0], LiteralType::i32);
  EXPECT_EQ(fn_type->arg_types[1], LiteralType::f32);
  EXPECT_EQ(fn_type->return_type, LiteralType::Bool);
  EXPECT_FALSE(fn_type->is_variadic);

  // Function with the same signature should resolve to the same TypeId.
  auto type_id = type_context.GetTypeIdFor(ParsedType{
      ParsedFunctionType{{ParsedType{"i32"}, ParsedType{"f32"}},
                         std::make_shared<ParsedType>(ParsedType{"bool"})}});
  ASSERT_TRUE(type_id.has_value());
  EXPECT_EQ(symbol->realized_type_id, type_id);
}

TEST_F(TypeContextTest, GetTypeIdFor_UnknownType) {
  auto type_id = type_context.GetTypeIdFor(ParsedType{"UnknownType"});
  EXPECT_FALSE(type_id.has_value());
}

TEST_F(TypeContextTest, GetTypeIdFor_UnionType) {
  ParsedUnionType union_type;
  union_type.names = {ParsedType{"i32"}, ParsedType{"f32"}};
  auto type_id = type_context.GetTypeIdFor(ParsedType{union_type});
  ASSERT_TRUE(type_id.has_value());
  auto union_info = type_registry.GetType<UnionType>(type_id.value());
  EXPECT_EQ(union_info->types.size(), 2);
  EXPECT_TRUE(std::find(union_info->types.begin(), union_info->types.end(),
                        LiteralType::i32) != union_info->types.end());
  EXPECT_TRUE(std::find(union_info->types.begin(), union_info->types.end(),
                        LiteralType::f32) != union_info->types.end());
}

TEST_F(TypeContextTest, GetTypeIdFor_FunctionType) {
  ParsedFunctionType fn_type;
  fn_type.arguments = {ParsedType{"i32"}, ParsedType{"f32"}};
  fn_type.return_value = std::make_shared<ParsedType>(ParsedType{"bool"});
  auto type_id = type_context.GetTypeIdFor(ParsedType{fn_type});
  ASSERT_TRUE(type_id.has_value());
  auto fn_info = type_registry.GetType<FunctionType>(type_id.value());
  EXPECT_EQ(fn_info->arg_types.size(), 2);
  EXPECT_EQ(fn_info->arg_types[0], LiteralType::i32);
  EXPECT_EQ(fn_info->arg_types[1], LiteralType::f32);
  EXPECT_EQ(fn_info->return_type, LiteralType::Bool);
  EXPECT_FALSE(fn_info->is_variadic);
  // Always resolves to the same TypeId.
  auto second_call = type_context.GetTypeIdFor(ParsedType{fn_type});
  ASSERT_TRUE(second_call.has_value());

  EXPECT_EQ(*type_id, *second_call);
}

TEST_F(TypeContextTest, GetTypeIdFor_FunctionType_VoidReturn) {
  ParsedFunctionType fn_type;
  fn_type.arguments = {ParsedType{"i32"}};
  // No return_value means Void
  auto type_id = type_context.GetTypeIdFor(ParsedType{fn_type});
  ASSERT_TRUE(type_id.has_value());
  auto fn_info = type_registry.GetType<FunctionType>(type_id.value());
  EXPECT_EQ(fn_info->return_type, LiteralType::Void);
}

TEST_F(TypeContextTest, DeclareStructSymbol_NoTemplate) {
  StructDeclaration declaration = {
      .name = "TestStruct",
  };

  auto symbol = type_registry.NewStructSymbol(declaration);
  // Realized TypeId should be assigned (and after built-in types).
  EXPECT_GE(symbol.realized_type_id, LiteralType::kCount);
  EXPECT_EQ(symbol.kind, NamedBinding::Struct);
  EXPECT_FALSE(symbol.idx.has_value());  // Structs have no index

  EXPECT_TRUE(scope_manager.FindBindingFor("TestStruct",
                                           ScopeManager::ScopeToCheck::All));
}

TEST_F(TypeContextTest, DeclareStructSymbol_WithTemplate) {
  StructDeclaration declaration = {
      .name = "TestStruct",
      .template_arguments = {{"T"}},
  };

  auto symbol = type_registry.NewStructSymbol(declaration);
  EXPECT_FALSE(symbol.realized_type_id.has_value());
  EXPECT_EQ(symbol.kind, NamedBinding::Struct);
  EXPECT_FALSE(symbol.idx.has_value());  // Structs have no index

  EXPECT_TRUE(scope_manager.FindBindingFor("TestStruct",
                                           ScopeManager::ScopeToCheck::All));
}

TEST_F(TypeContextTest, DefineStructType_NoTemplate) {
  StructDeclaration declaration;
  declaration.name = "TestStruct";
  declaration.fields = {{"field1", ParsedType{"i32"}},
                        {"field2", ParsedType{"f32"}}};
  declaration.is_extern = false;

  auto binding = type_registry.NewStructSymbol(declaration);
  ASSERT_TRUE(binding.realized_type_id.has_value());
  ASSERT_TRUE(binding.symbol_id.has_value());

  auto* const struct_symbol =
      type_registry.GetSymbol<StructSymbol>(*binding.symbol_id);
  ASSERT_TRUE(struct_symbol);
  type_context.DefineStructType(*binding.realized_type_id, *struct_symbol,
                                /*template_arguments=*/{});

  // Check that struct type is registered
  auto struct_info =
      type_registry.GetType<StructType>(*binding.realized_type_id);
  EXPECT_EQ(struct_info->declaration.name, "TestStruct");
  EXPECT_THAT(struct_info->field_types,
              testing::ElementsAre(LiteralType::i32, LiteralType::f32));

  auto field1_binding = scope_manager.FindBindingFor(
      "field1", ScopeManager::ScopeToCheck::Current,
      /*override_scope_id=*/struct_info->scope_id);

  ASSERT_TRUE(field1_binding.has_value());
  EXPECT_EQ(field1_binding->kind, NamedBinding::Field);
  EXPECT_EQ(field1_binding->idx, 0);
  EXPECT_EQ(field1_binding->name, "field1");
  EXPECT_EQ(field1_binding->realized_type_id, LiteralType::i32);

  auto field2_binding = scope_manager.FindBindingFor(
      "field2", ScopeManager::ScopeToCheck::Current,
      /*override_scope_id=*/struct_info->scope_id);

  ASSERT_TRUE(field2_binding.has_value());
  EXPECT_EQ(field2_binding->kind, NamedBinding::Field);
  EXPECT_EQ(field2_binding->idx, 1);
  EXPECT_EQ(field2_binding->name, "field2");
  EXPECT_EQ(field2_binding->realized_type_id, LiteralType::f32);
}

TEST_F(TypeContextTest, TemplateStruct_GetTemplateOf) {
  StructDeclaration declaration;
  declaration.name = "TestStruct";
  declaration.template_arguments = {{"T"}};
  declaration.fields = {{"field1", ParsedType{"T"}}};
  declaration.is_extern = false;

  auto binding = type_registry.NewStructSymbol(declaration);
  ASSERT_FALSE(binding.realized_type_id.has_value());
  ASSERT_TRUE(binding.symbol_id.has_value());

  std::optional<TypeId> type_id =
      type_context.GetTemplateOf(binding, {LiteralType::Bool});
  ASSERT_TRUE(type_id.has_value());

  // Check that struct type is registered
  auto struct_info = type_registry.GetType<StructType>(*type_id);
  EXPECT_EQ(struct_info->declaration.name, "TestStruct");
  EXPECT_THAT(struct_info->field_types,
              testing::ElementsAre(LiteralType::Bool));

  auto field1_binding = scope_manager.FindBindingFor(
      "field1", ScopeManager::ScopeToCheck::Current,
      /*override_scope_id=*/struct_info->scope_id);

  ASSERT_TRUE(field1_binding.has_value());
  EXPECT_EQ(field1_binding->kind, NamedBinding::Field);
  EXPECT_EQ(field1_binding->idx, 0);
  EXPECT_EQ(field1_binding->name, "field1");
  EXPECT_EQ(field1_binding->realized_type_id, LiteralType::Bool);
}

TEST_F(TypeContextTest, TemplateFunction_GetTemplateOf) {
  FunctionDeclaration declaration;
  declaration.name = "TestFunction";
  declaration.arguments = {{"arg1", ParsedType{"Type"}}};
  declaration.return_type = ParsedType{"Return"};
  declaration.function_kind = FunctionKind::Free;
  declaration.template_arguments = {{"Type"}, {"Return"}};
  declaration.body = std::make_unique<Block>();

  auto symbol_id = type_registry.NewFunctionSymbol(declaration);
  auto binding = type_context.DefineFunction(symbol_id);
  ASSERT_TRUE(binding.has_value());

  EXPECT_FALSE(binding->realized_type_id.has_value());
  EXPECT_TRUE(binding->symbol_id.has_value());

  std::optional<TypeId> type_id = type_context.GetTemplateOf(
      *binding, {LiteralType::Bool, LiteralType::i32});
  ASSERT_TRUE(type_id.has_value());

  auto fn_info = type_registry.GetType<FunctionType>(*type_id);
  EXPECT_THAT(fn_info->arg_types, testing::ElementsAre(LiteralType::Bool));
  EXPECT_EQ(fn_info->return_type, LiteralType::i32);
}

TEST_F(TypeContextTest, StructDeclaration_WithMethod) {
  FunctionDeclaration method_decl = {
      .name = "test_method",
      .arguments = {{"self", ParsedType{"TestStruct"}},
                    {"arg", ParsedType{"i32"}}},
      .return_type = ParsedType{"Void"},
      .function_kind = FunctionKind::Method,
      .body = std::make_unique<Block>(),
  };

  StructDeclaration struct_decl;
  struct_decl.name = "TestStruct";
  struct_decl.methods.emplace_back("test_method", std::move(method_decl));
  struct_decl.is_extern = false;

  auto binding = type_registry.NewStructSymbol(struct_decl);
  ASSERT_TRUE(binding.realized_type_id.has_value());
  ASSERT_TRUE(binding.symbol_id.has_value());

  auto* const struct_symbol =
      type_registry.GetSymbol<StructSymbol>(*binding.symbol_id);
  ASSERT_TRUE(struct_symbol);
  type_context.DefineStructType(*binding.realized_type_id, *struct_symbol,
                                /*template_arguments=*/{});

  auto struct_info =
      type_registry.GetType<StructType>(*binding.realized_type_id);

  auto method_binding = scope_manager.FindBindingFor(
      "test_method", ScopeManager::ScopeToCheck::Current,
      /*override_scope_id=*/struct_info->scope_id);

  ASSERT_TRUE(method_binding.has_value());
  EXPECT_EQ(method_binding->kind, NamedBinding::Function);
  EXPECT_EQ(method_binding->name, "test_method");
  EXPECT_TRUE(method_binding->realized_type_id.has_value());
}

TEST_F(TypeContextTest, DefineFunction_ExternMethod) {
  StructDeclaration struct_decl;
  struct_decl.name = "String";
  struct_decl.is_extern = true;

  auto struct_binding = type_registry.NewStructSymbol(struct_decl);
  ASSERT_TRUE(struct_binding.realized_type_id.has_value());
  ASSERT_TRUE(struct_binding.symbol_id.has_value());

  auto* const struct_symbol =
      type_registry.GetSymbol<StructSymbol>(*struct_binding.symbol_id);
  ASSERT_TRUE(struct_symbol);
  type_context.DefineStructType(*struct_binding.realized_type_id,
                                *struct_symbol,
                                /*template_arguments=*/{});

  FunctionDeclaration method_decl{
      .name = "length",
      .arguments = {{"self", ParsedType{"String"}}},
      .return_type = ParsedType{"i32"},
      .function_kind = FunctionKind::Method,
  };

  auto method_symbol_id = type_registry.NewFunctionSymbol(method_decl);
  auto method_binding = type_context.DefineFunction(
      method_symbol_id, *struct_binding.realized_type_id);
  ASSERT_TRUE(method_binding.has_value());
  EXPECT_EQ(method_binding->kind, NamedBinding::Function);
  EXPECT_EQ(method_binding->symbol_id, method_symbol_id);
}

TEST_F(TypeContextTest, IsTypeSubsetOf) {
  // Equivalent types
  EXPECT_TRUE(type_context.IsTypeSubsetOf(LiteralType::i32, LiteralType::i32));

  // Any type
  EXPECT_TRUE(type_context.IsTypeSubsetOf(LiteralType::i32, LiteralType::Any));
  EXPECT_TRUE(type_context.IsTypeSubsetOf(LiteralType::Bool, LiteralType::Any));

  // Union types
  ParsedUnionType union_type;
  union_type.names = {ParsedType{"i32"}, ParsedType{"f32"}, ParsedType{"bool"}};
  auto union_id = type_context.GetTypeIdFor(ParsedType{union_type});
  ASSERT_TRUE(union_id.has_value());

  StructDeclaration test_struct_decl;
  test_struct_decl.name = "TestStruct";
  test_struct_decl.is_extern = true;

  NamedBinding struct_binding = type_registry.NewStructSymbol(test_struct_decl);
  ASSERT_TRUE(struct_binding.realized_type_id.has_value());
  ASSERT_TRUE(struct_binding.symbol_id.has_value());
  TypeId struct_type_id = *struct_binding.realized_type_id;

  auto* const struct_symbol =
      type_registry.GetSymbol<StructSymbol>(*struct_binding.symbol_id);
  ASSERT_TRUE(struct_symbol);
  type_context.DefineStructType(struct_type_id, *struct_symbol,
                                /*template_arguments=*/{});

  EXPECT_TRUE(type_context.IsTypeSubsetOf(LiteralType::i32, union_id.value()));
  EXPECT_TRUE(type_context.IsTypeSubsetOf(LiteralType::Bool, union_id.value()));
  EXPECT_FALSE(type_context.IsTypeSubsetOf(*struct_binding.realized_type_id,
                                           union_id.value()));

  // Union subset of union
  ParsedUnionType sub_union;
  sub_union.names = {ParsedType{"i32"}, ParsedType{"f32"}};
  auto sub_union_id = type_context.GetTypeIdFor(ParsedType{sub_union});
  ASSERT_TRUE(sub_union_id.has_value());

  EXPECT_TRUE(
      type_context.IsTypeSubsetOf(sub_union_id.value(), union_id.value()));
  EXPECT_FALSE(
      type_context.IsTypeSubsetOf(union_id.value(), sub_union_id.value()));

  ParsedUnionType new_union;
  new_union.names = {ParsedType{"TestStruct"}, ParsedType{sub_union}};
  auto new_union_id = type_context.GetTypeIdFor(ParsedType{new_union});
  ASSERT_TRUE(new_union_id.has_value());

  EXPECT_TRUE(
      type_context.IsTypeSubsetOf(LiteralType::i32, new_union_id.value()));
  EXPECT_TRUE(
      type_context.IsTypeSubsetOf(struct_type_id, new_union_id.value()));
  EXPECT_FALSE(
      type_context.IsTypeSubsetOf(LiteralType::Bool, new_union_id.value()));
}

TEST_F(TypeContextTest, GetNameFromTypeId) {
  EXPECT_EQ(type_registry.GetNameFromTypeId(LiteralType::Void), "Void");
  EXPECT_EQ(type_registry.GetNameFromTypeId(LiteralType::i32), "i32");
  EXPECT_EQ(type_registry.GetNameFromTypeId(LiteralType::f32), "f32");
  EXPECT_EQ(type_registry.GetNameFromTypeId(LiteralType::Bool), "bool");
  EXPECT_EQ(type_registry.GetNameFromTypeId(LiteralType::Any), "any");

  // Function type
  FunctionDeclaration fn_decl{
      .name = "test_fn",
      .arguments = {{"arg1", ParsedType{"i32"}}},
      .return_type = ParsedType{"bool"},
      .function_kind = FunctionKind::Free,
      .body = std::make_unique<Block>(),
  };
  auto symbol_id = type_registry.NewFunctionSymbol(fn_decl);
  auto symbol = type_context.DefineFunction(symbol_id);
  ASSERT_TRUE(symbol.has_value());
  ASSERT_TRUE(symbol->realized_type_id.has_value());
  std::string fn_name =
      type_registry.GetNameFromTypeId(*symbol->realized_type_id);
  EXPECT_EQ(fn_name, "fn (i32) -> bool");

  // Variadic function
  FunctionDeclaration variadic_fn{
      .name = "log",
      .arguments = {{"arg1", ParsedType{"i32"}}},
      .return_type = ParsedType{"bool"},
      .function_kind = FunctionKind::Extern,
      .is_variadic = true,
      .body = std::make_unique<Block>(),
  };
  auto variadic_symbol_id = type_registry.NewFunctionSymbol(variadic_fn);
  auto variadic_symbol = type_context.DefineFunction(variadic_symbol_id);
  ASSERT_TRUE(variadic_symbol.has_value());
  ASSERT_TRUE(variadic_symbol->realized_type_id.has_value());
  std::string variadic_name =
      type_registry.GetNameFromTypeId(*variadic_symbol->realized_type_id);
  EXPECT_EQ(variadic_name, "fn (i32, ...) -> bool");

  // Struct type
  StructDeclaration struct_decl;
  struct_decl.name = "TestStruct";
  struct_decl.is_extern = false;
  auto struct_binding = type_registry.NewStructSymbol(struct_decl);
  ASSERT_TRUE(struct_binding.realized_type_id.has_value());
  auto* const struct_symbol =
      type_registry.GetSymbol<StructSymbol>(*struct_binding.symbol_id);
  ASSERT_TRUE(struct_symbol);
  type_context.DefineStructType(*struct_binding.realized_type_id,
                                *struct_symbol,
                                /*template_arguments=*/{});
  EXPECT_EQ(type_registry.GetNameFromTypeId(*struct_binding.realized_type_id),
            "struct TestStruct");

  // Union type
  ParsedUnionType union_type;
  union_type.names = {ParsedType{"i32"}, ParsedType{"f32"}, ParsedType{"i32"}};
  auto union_id = type_context.GetTypeIdFor(ParsedType{union_type});
  ASSERT_TRUE(union_id.has_value());
  std::string union_name = type_registry.GetNameFromTypeId(union_id.value());
  EXPECT_EQ(union_name, "Union[i32, f32]");
}

TEST_F(TypeContextTest, GetTypeIdFor_Never) {
  std::optional<TypeId> type_id =
      type_context.GetTypeIdFor(ParsedType{"never"});
  ASSERT_TRUE(type_id.has_value());
  EXPECT_EQ(type_id, LiteralType::Never);

  std::optional<TypeId> union_type_id = type_context.GetTypeIdFor(
      ParsedType{ParsedUnionType{{{"bool"}, {"i32"}}}});
  ASSERT_TRUE(union_type_id.has_value());

  std::optional<TypeId> union_type_id_with_never = type_context.GetTypeIdFor(
      ParsedType{ParsedUnionType{{{"bool"}, {"i32"}, {"never"}}}});
  ASSERT_TRUE(union_type_id_with_never.has_value());

  EXPECT_EQ(union_type_id, union_type_id_with_never);
}

TEST_F(TypeContextTest, GetTypeIdFor_Nil) {
  std::optional<TypeId> type_id = type_context.GetTypeIdFor(ParsedType{"Nil"});
  EXPECT_FALSE(type_id.has_value());

  std::optional<TypeId> union_type_id = type_context.GetTypeIdFor(
      ParsedType{ParsedUnionType{{{"bool"}, {"Nil"}}}});
  EXPECT_FALSE(union_type_id.has_value());
}

// TEST_F(TypeContextTest, GetCurrentFunction) {
//   // In global scope, GetCurrentFunction should return "main"
//   auto& main_fn = type_context.GetCurrentFunction();
//   EXPECT_EQ(main_fn.name, "<<main>>");
//   EXPECT_EQ(main_fn.function_kind, FunctionKind::Free);

//   // Define a new function and enter its scope
//   FunctionDeclaration fn_decl{
//       .name = "test_fn",
//       .arguments = {},
//       .return_type = ParsedType{"Void"},
//       .function_kind = FunctionKind::Free,
//       .body = std::make_unique<Block>(),
//   };
//   auto symbol = type_context.DefineFunction(fn_decl, &error_collector);
//   ASSERT_TRUE(symbol.has_value());
//   type_context.EnterScope(TypeContext::ScopeType::FunctionScope, &fn_decl);

//   auto& current_fn = type_context.GetCurrentFunction();
//   EXPECT_EQ(current_fn.name, "test_fn");
// }

TEST_F(TypeContextTest, FunctionType_OperatorEqual) {
  FunctionType ft1{
      {LiteralType::i32, LiteralType::f32}, LiteralType::Bool, false};
  FunctionType ft2{
      {LiteralType::i32, LiteralType::f32}, LiteralType::Bool, false};
  FunctionType ft3{{LiteralType::i32}, LiteralType::Bool, false};

  EXPECT_TRUE(ft1 == ft2);
  EXPECT_FALSE(ft1 == ft3);
}

TEST_F(TypeContextTest, UnionType_OperatorEqual) {
  UnionType ut1{{LiteralType::i32, LiteralType::f32}};
  UnionType ut2{{LiteralType::i32, LiteralType::f32}};
  UnionType ut3{{LiteralType::i32, LiteralType::Bool}};

  EXPECT_TRUE(ut1 == ut2);
  EXPECT_FALSE(ut1 == ut3);
}

TEST_F(TypeContextTest, FunctionType_Hash) {
  FunctionType::Hash hasher;
  FunctionType ft1{{LiteralType::i32}, LiteralType::Bool, false};
  FunctionType ft2{{LiteralType::i32}, LiteralType::Bool, false};
  FunctionType ft3{{LiteralType::f32}, LiteralType::Bool, false};
  FunctionType ft4{{LiteralType::i32}, LiteralType::Bool, true};

  EXPECT_EQ(hasher(ft1), hasher(ft2));
  EXPECT_NE(hasher(ft1), hasher(ft3));
  EXPECT_NE(hasher(ft1), hasher(ft4));
}

TEST_F(TypeContextTest, UnionType_Hash) {
  UnionType::Hash hasher;
  UnionType ut1{{LiteralType::i32, LiteralType::f32}};
  UnionType ut2{{LiteralType::i32, LiteralType::f32}};
  UnionType ut3{{LiteralType::i32, LiteralType::Bool}};

  EXPECT_EQ(hasher(ut1), hasher(ut2));
  EXPECT_NE(hasher(ut1), hasher(ut3));
}

}  // namespace