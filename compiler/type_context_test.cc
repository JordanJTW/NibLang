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
#include "symbol_binder.h"

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::Return;

namespace {

using LiteralType = TypeRegistry::LiteralType;

class TypeContextTest : public ::testing::Test {
 protected:
  ErrorCollector error_collector;
  ScopeManager scope_manager{error_collector};
  TypeRegistry type_registry{scope_manager};
  TypeContext type_context{scope_manager, type_registry, error_collector};
  SymbolBinder symbol_binder{scope_manager, type_registry, type_context,
                             error_collector};

  // TypeId MakeStruct(std::string name,
  //                   StructDeclaration::Kind kind,
  //                   const std::vector<std::string>& implements = {});

 private:
  // std::deque ensures insertion do not invalidate pointers unlike std::vector
  std::deque<StructDeclaration> nameable_declarations_;
};

// TypeId TypeContextTest::MakeStruct(std::string name,
//                                    StructDeclaration::Kind kind,
//                                    const std::vector<std::string>&
//                                    implements) {
//   StructDeclaration& declaration = nameable_declarations_.emplace_back();
//
//   declaration.name = SpannedText{std::move(name)};
//   declaration.kind = kind;
//
//   declaration.interfaces.reserve(implements.size());
//   for (const auto& impl_name : implements) {
//     auto impl_name_span = SpannedText{impl_name};
//     declaration.interfaces.emplace_back(
//         impl_name_span, ImplementsDeclaration{.name = impl_name_span});
//   }
//
//   const auto& [binding, symbol] =
//   symbol_binder.ForwardDeclareStruct(declaration);
//   CHECK(binding.symbol_id.has_value());
//
//   type_context.DefineStructType(binding.type_id, *binding.symbol_id,
//                                 *symbol,
//                                 /*template_arguments=*/{});
//   return *binding.type_id;
// }

TEST_F(TypeContextTest, GetTypeIdFor_BuiltInType) {
  static const std::unordered_map<std::string, TypeId> kBuiltInTypes = {
      {"Unit", LiteralType::Unit}, {"i32", LiteralType::i32},
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
      .name = SpannedText{"test_fn"},
      .arguments = {{SpannedText{"arg1"}, ParsedType{"i32"}},
                    {SpannedText{"arg2"}, ParsedType{"f32"}}},
      .return_type = ParsedType{"bool"},
      .function_kind = FunctionKind::Free,
      .body = std::make_unique<Block>(),  // non-extern MUST have body
  };

  auto symbol_id = type_registry.NewFunctionSymbol(fn_decl);
  auto symbol = type_context.DefineFunction(symbol_id);
  ASSERT_TRUE(symbol.has_value());
  EXPECT_EQ(symbol->kind, NamedBinding::Function);

  // Check that the function type is registered
  auto fn_type = type_registry.GetType<FunctionType>(symbol->type_id);
  EXPECT_EQ(fn_type->arg_types.size(), 2);
  EXPECT_EQ(fn_type->arg_types[0], LiteralType::i32);
  EXPECT_EQ(fn_type->arg_types[1], LiteralType::f32);
  EXPECT_EQ(fn_type->return_type, LiteralType::Bool);
  EXPECT_FALSE(fn_type->variadic_type.has_value());

  // Function with the same signature should resolve to the same TypeId.
  auto type_id = type_context.GetTypeIdFor(ParsedType{
      ParsedFunctionType{{ParsedType{"i32"}, ParsedType{"f32"}},
                         std::make_shared<ParsedType>(ParsedType{"bool"})}});
  ASSERT_TRUE(type_id.has_value());
  EXPECT_EQ(symbol->type_id, type_id);
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
  EXPECT_FALSE(fn_info->variadic_type.has_value());
  // Always resolves to the same TypeId.
  auto second_call = type_context.GetTypeIdFor(ParsedType{fn_type});
  ASSERT_TRUE(second_call.has_value());

  EXPECT_EQ(*type_id, *second_call);
}

TEST_F(TypeContextTest, GetTypeIdFor_FunctionType_NoReturn) {
  ParsedFunctionType fn_type;
  fn_type.arguments = {ParsedType{"i32"}};
  // No return_value implies Unit()
  auto type_id = type_context.GetTypeIdFor(ParsedType{fn_type});
  ASSERT_TRUE(type_id.has_value());
  auto fn_info = type_registry.GetType<FunctionType>(type_id.value());
  EXPECT_EQ(fn_info->return_type, LiteralType::Unit);
}

TEST_F(TypeContextTest, DISABLED_DeclareStructSymbol_NoTemplate) {
  // StructDeclaration declaration = {
  //     .name = SpannedText{"TestStruct"},
  // };
  //
  // auto symbol = type_registry.NewStructSymbol(declaration);
  // // Realized TypeId should be assigned (and after built-in types).
  // EXPECT_GE(symbol.realized_type_id, LiteralType::kCount);
  // EXPECT_EQ(symbol.kind, NamedBinding::Struct);
  // EXPECT_FALSE(symbol.idx.has_value());  // Structs have no index
  //
  // EXPECT_TRUE(scope_manager.FindBindingFor("TestStruct",
  //                                          ScopeManager::ScopeToCheck::All));
}

TEST_F(TypeContextTest, DISABLED_DeclareStructSymbol_WithTemplate) {
  // StructDeclaration declaration = {
  //     .name = SpannedText{"TestStruct"},
  //     .template_arguments = {{"T"}},
  // };
  //
  // auto symbol = type_registry.NewStructSymbol(declaration);
  // EXPECT_FALSE(symbol.realized_type_id.has_value());
  // EXPECT_EQ(symbol.kind, NamedBinding::Struct);
  // EXPECT_FALSE(symbol.idx.has_value());  // Structs have no index
  //
  // EXPECT_TRUE(scope_manager.FindBindingFor("TestStruct",
  //                                          ScopeManager::ScopeToCheck::All));
}

// TEST_F(TypeContextTest, DefineStructType_NoTemplate) {
//   StructDeclaration declaration;
//   declaration.name = SpannedText{"TestStruct"};
//   declaration.fields = {{SpannedText{"field1"}, ParsedType{"i32"}},
//                         {SpannedText{"field2"}, ParsedType{"f32"}}};
//   declaration.kind = StructDeclaration::Structure;
//
//   const auto& [binding, symbol] =
//   symbol_binder.ForwardDeclareStruct(declaration);
//   ASSERT_TRUE(binding.type_id.has_value());
//   ASSERT_TRUE(binding.symbol_id.has_value());
//
//   type_context.DefineStructType(*binding.type_id, *binding.symbol_id,
//                                 *symbol,
//                                 /*template_arguments=*/{});
//
//   // Check that struct type is registered
//   auto struct_info =
//       type_registry.GetType<StructType>(*binding.type_id);
//   EXPECT_EQ(struct_info->declaration.name.text, "TestStruct");
//   EXPECT_THAT(struct_info->field_types,
//               testing::ElementsAre(LiteralType::i32, LiteralType::f32));
//
//   auto field1_binding = scope_manager.FindBindingFor(
//       "field1", ScopeManager::ScopeToCheck::Current,
//       /*override_scope_id=*/struct_info->scope_id);
//
//   ASSERT_TRUE(field1_binding.has_value());
//   EXPECT_EQ(field1_binding->kind, NamedBinding::Field);
//   EXPECT_EQ(field1_binding->idx, 0);
//   EXPECT_EQ(field1_binding->name.text, "field1");
//   EXPECT_EQ(field1_binding->type_id, LiteralType::i32);
//
//   auto field2_binding = scope_manager.FindBindingFor(
//       "field2", ScopeManager::ScopeToCheck::Current,
//       /*override_scope_id=*/struct_info->scope_id);
//
//   ASSERT_TRUE(field2_binding.has_value());
//   EXPECT_EQ(field2_binding->kind, NamedBinding::Field);
//   EXPECT_EQ(field2_binding->idx, 1);
//   EXPECT_EQ(field2_binding->name.text, "field2");
//   EXPECT_EQ(field2_binding->type_id, LiteralType::f32);
// }

// TEST_F(TypeContextTest, GetTemplateOf_Struct) {
//   StructDeclaration declaration;
//   declaration.name = SpannedText{"TestStruct"};
//   declaration.template_variables = {{"T"}};
//   declaration.fields = {{SpannedText{"field1"}, ParsedType{"T"}}};
//   declaration.kind = StructDeclaration::Structure;
//
//   const auto& [binding, symbol] =
//   symbol_binder.ForwardDeclareStruct(declaration);
//   ASSERT_FALSE(binding.type_id.has_value());
//   ASSERT_TRUE(binding.symbol_id.has_value());
//
//   std::optional<TypeId> type_id = type_context.GetTemplateOf(
//       binding, {LiteralType::Bool}, /*argument_spans=*/{{}});
//   ASSERT_TRUE(type_id.has_value());
//
//   // Check that struct type is registered
//   auto struct_info = type_registry.GetType<StructType>(*type_id);
//   EXPECT_EQ(struct_info->declaration.name.text, "TestStruct");
//   EXPECT_THAT(struct_info->field_types,
//               testing::ElementsAre(LiteralType::Bool));
//
//   auto field1_binding = scope_manager.FindBindingFor(
//       "field1", ScopeManager::ScopeToCheck::Current,
//       /*override_scope_id=*/struct_info->scope_id);
//
//   ASSERT_TRUE(field1_binding.has_value());
//   EXPECT_EQ(field1_binding->kind, NamedBinding::Field);
//   EXPECT_EQ(field1_binding->idx, 0);
//   EXPECT_EQ(field1_binding->name.text, "field1");
//   EXPECT_EQ(field1_binding->type_id, LiteralType::Bool);
// }

TEST_F(TypeContextTest, GetTemplateOf_Function) {
  FunctionDeclaration declaration;
  declaration.name = SpannedText{"TestFunction"};
  declaration.arguments = {{SpannedText{"arg1"}, ParsedType{"Type"}}};
  declaration.return_type = ParsedType{"Return"};
  declaration.function_kind = FunctionKind::Free;
  declaration.template_variables = {{"Type"}, {"Return"}};
  declaration.body = std::make_unique<Block>();

  auto symbol_id = type_registry.NewFunctionSymbol(declaration);
  auto binding = type_context.DefineFunction(symbol_id);
  ASSERT_TRUE(binding.has_value());

  EXPECT_TRUE(binding->symbol_id.has_value());

  std::optional<TypeId> type_id = type_context.GetTemplateOf(
      *binding, {LiteralType::Bool, LiteralType::i32},
      /*argument_spans=*/{{}, {}});
  ASSERT_TRUE(type_id.has_value());

  auto fn_info = type_registry.GetType<FunctionType>(*type_id);
  EXPECT_THAT(fn_info->arg_types, testing::ElementsAre(LiteralType::Bool));
  EXPECT_EQ(fn_info->return_type, LiteralType::i32);
}

// TEST_F(TypeContextTest, GetTemplateOf_Nested) {
//   auto make_template_type = [](std::string base, ParsedType parameter) {
//     return ParsedType{ParsedParameterizedType{
//         std::make_shared<ParsedType>(base), {std::move(parameter)}}};
//   };
//
//   // struct Box[T] { value: T; fn Get() -> T; }
//   StructDeclaration box_declaration;
//   box_declaration.name = SpannedText{"Box"};
//   box_declaration.template_variables = {{"T"}};
//   box_declaration.fields = {{SpannedText{"value"}, ParsedType{"T"}}};
//   box_declaration.methods.emplace_back(
//       SpannedText{"Get"},
//       FunctionDeclaration{.name = SpannedText{"Get"},
//                           .arguments = {},
//                           .return_type = ParsedType{"T"},
//                           .function_kind = FunctionKind::Method,
//                           .template_variables = {},
//                           .body = std::make_unique<Block>()});
//   box_declaration.kind = StructDeclaration::Structure;
//
//   const auto& [box_binding, box_symbol] =
//       symbol_binder.ForwardDeclareStruct(box_declaration);
//   ASSERT_TRUE(box_binding.symbol_id.has_value());
//
//   // struct Array[T] { value: Box[T]; fn Push[R](value: T) -> Box[R]; }
//   StructDeclaration array_declaration;
//   array_declaration.name = SpannedText{"Array"};
//   array_declaration.template_variables = {{"T"}};
//   array_declaration.fields = {
//       {SpannedText{"value"}, make_template_type("Box", ParsedType{"T"})}};
//   array_declaration.methods.emplace_back(
//       SpannedText{"Push"},
//       FunctionDeclaration{
//           .name = SpannedText{"Push"},
//           .arguments = {{SpannedText{"value"}, ParsedType{"T"}}},
//           .return_type = make_template_type("Box", ParsedType{"R"}),
//           .function_kind = FunctionKind::Method,
//           .template_variables = {{"R"}},
//           .body = std::make_unique<Block>()});
//   array_declaration.kind = StructDeclaration::Structure;
//
//   const auto& [array_binding, array_symbol] =
//       symbol_binder.ForwardDeclareStruct(array_declaration);
//   ASSERT_TRUE(array_binding.symbol_id.has_value());
//
//   // fn DoIt[T](arg: Array[Box[T]]) -> Array[i32];
//   FunctionDeclaration declaration;
//   declaration.name = SpannedText{"DoIt"};
//   declaration.arguments = {
//       {SpannedText{"arg"},
//        make_template_type("Array",
//                           make_template_type("Box", ParsedType{"T"}))}};
//   declaration.return_type = make_template_type("Array", ParsedType{"i32"});
//   declaration.function_kind = FunctionKind::Free;
//   declaration.template_variables = {{"T"}};
//   declaration.body = std::make_unique<Block>();
//
//   auto symbol_id = type_registry.NewFunctionSymbol(declaration);
//   auto binding = type_context.DefineFunction(symbol_id);
//   ASSERT_TRUE(binding.has_value());
//
//   // Instantiates: DoIt[i32](arg: Array[Box[i32]]) -> Array[i32];
//   std::optional<TypeId> fn_type_id = type_context.GetTemplateOf(
//       *binding, {LiteralType::i32}, /*argument_spans=*/{{}});
//   ASSERT_TRUE(fn_type_id.has_value());
//
//   // Instantiates: Array[Box[bool]]::Push[Array[Box[bool]]];
//   std::optional<TypeId> struct_type_id = type_context.GetTemplateOf(
//       array_binding, {LiteralType::Bool}, /*argument_spans=*/{{}});
//   ASSERT_TRUE(struct_type_id.has_value());
//
//   const auto* struct_type = type_registry.GetType<StructType>(*struct_type_id);
//   ASSERT_TRUE(struct_type);
//
//   std::optional<NamedBinding> push_binding = scope_manager.FindBindingFor(
//       "Push", ScopeManager::Current, struct_type->scope_id);
//   ASSERT_TRUE(push_binding.has_value());
//
//   std::optional<TypeId> push_type_id = type_context.GetTemplateOf(
//       *push_binding, {*struct_type_id}, /*argument_spans=*/{{}});
//   ASSERT_TRUE(push_type_id.has_value());
// }

// TEST_F(TypeContextTest, StructDeclaration_WithMethod) {
//   FunctionDeclaration method_decl = {
//       .name = SpannedText{"test_method"},
//       .arguments = {{SpannedText{"self"}, ParsedType{"TestStruct"}},
//                     {SpannedText{"arg"}, ParsedType{"i32"}}},
//       .return_type = ParsedType{"Unit"},
//       .function_kind = FunctionKind::Method,
//       .body = std::make_unique<Block>(),
//   };
//
//   StructDeclaration struct_decl;
//   struct_decl.name = SpannedText{"TestStruct"};
//   struct_decl.methods.emplace_back("test_method", std::move(method_decl));
//   struct_decl.kind = StructDeclaration::Structure;
//
//   const auto& [binding, symbol] =
//       symbol_binder.ForwardDeclareStruct(struct_decl);
//   ASSERT_TRUE(binding.type_id.has_value());
//   ASSERT_TRUE(binding.symbol_id.has_value());
//
//   type_context.DefineStructType(*binding.type_id, *binding.symbol_id, *symbol,
//                                 /*template_arguments=*/{});
//
//   auto struct_info = type_registry.GetType<StructType>(*binding.type_id);
//
//   auto method_binding = scope_manager.FindBindingFor(
//       "test_method", ScopeManager::ScopeToCheck::Current,
//       /*override_scope_id=*/struct_info->scope_id);
//
//   ASSERT_TRUE(method_binding.has_value());
//   EXPECT_EQ(method_binding->kind, NamedBinding::Function);
//   EXPECT_EQ(method_binding->name.text, "test_method");
//   EXPECT_TRUE(method_binding->type_id.has_value());
// }
//
// TEST_F(TypeContextTest, DefineFunction_ExternMethod) {
//   StructDeclaration struct_decl;
//   struct_decl.name = SpannedText{"String"};
//   struct_decl.kind = StructDeclaration::Opaque;
//
//   const auto& [binding, symbol] =
//       symbol_binder.ForwardDeclareStruct(struct_decl);
//   ASSERT_TRUE(binding.type_id.has_value());
//   ASSERT_TRUE(binding.symbol_id.has_value());
//
//   type_context.DefineStructType(*binding.type_id, *binding.symbol_id, *symbol,
//                                 /*template_arguments=*/{});
//
//   FunctionDeclaration method_decl{
//       .name = SpannedText{"length"},
//       .arguments = {{SpannedText{"self"}, ParsedType{"String"}}},
//       .return_type = ParsedType{"i32"},
//       .function_kind = FunctionKind::Method,
//   };
//
//   auto method_symbol_id =
//       type_registry.NewFunctionSymbol(method_decl, &struct_decl);
//   auto method_binding =
//       type_context.DefineFunction(method_symbol_id, *binding.type_id);
//   ASSERT_TRUE(method_binding.has_value());
//   EXPECT_EQ(method_binding->kind, NamedBinding::Function);
//   EXPECT_EQ(method_binding->symbol_id, method_symbol_id);
// }

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

  // StructDeclaration test_struct_decl;
  // test_struct_decl.name = SpannedText{"TestStruct"};
  // test_struct_decl.kind = StructDeclaration::Opaque;

  // const auto& [binding, symbol] =
  //     symbol_binder.ForwardDeclareStruct(test_struct_decl);
  // ASSERT_TRUE(binding.type_id.has_value());
  // ASSERT_TRUE(binding.symbol_id.has_value());
  // TypeId struct_type_id = binding.type_id;
  //
  // auto* const struct_symbol =
  //     type_registry.GetSymbol<StructSymbol>(*binding.symbol_id);
  // ASSERT_TRUE(struct_symbol);
  // type_context.DefineStructType(struct_type_id, *binding.symbol_id,
  //                               *struct_symbol,
  //                               /*template_arguments=*/{});

  EXPECT_TRUE(type_context.IsTypeSubsetOf(LiteralType::i32, union_id.value()));
  EXPECT_TRUE(type_context.IsTypeSubsetOf(LiteralType::Bool, union_id.value()));
  // EXPECT_FALSE(type_context.IsTypeSubsetOf(*binding.type_id, union_id.value()));

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
  // EXPECT_TRUE(
  //     type_context.IsTypeSubsetOf(struct_type_id, new_union_id.value()));
  EXPECT_FALSE(
      type_context.IsTypeSubsetOf(LiteralType::Bool, new_union_id.value()));
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

TEST_F(TypeContextTest, GetIntersectionOf_Absorption) {
  TypeId union_type =
      type_context.GetUnionOf({LiteralType::i32, LiteralType::Bool});
  TypeId intersection_type =
      type_context.GetIntersectionOf({union_type, LiteralType::i32});

  // Absorption Rule: (A & (A | B)) => A
  EXPECT_EQ(intersection_type, LiteralType::i32);
}

TEST_F(TypeContextTest, GetUnionOf_Absorption) {
  TypeId intersection_type =
      type_context.GetIntersectionOf({LiteralType::i32, LiteralType::Bool});
  TypeId union_type =
      type_context.GetUnionOf({intersection_type, LiteralType::i32});

  // Absorption Rule: (A | (A & B)) => A
  EXPECT_EQ(union_type, LiteralType::i32);
}

TEST_F(TypeContextTest, GetIntersectionOf_Never) {
  TypeId intersection_type =
      type_context.GetIntersectionOf({LiteralType::i32, LiteralType::Never});

  // Intersection with an empty set: `T & Never => Never`
  EXPECT_EQ(intersection_type, LiteralType::Never);
}

TEST_F(TypeContextTest, GetUnionOf_Never) {
  TypeId union_type =
      type_context.GetUnionOf({LiteralType::i32, LiteralType::Never});

  // Union with an empty set: `T | Never => T`
  EXPECT_EQ(union_type, LiteralType::i32);
}

TEST_F(TypeContextTest, GetIntersectionOf_DisjointTypes) {
  TypeId intersection_type =
      type_context.GetIntersectionOf({LiteralType::i32, LiteralType::Bool});

  // No overlap within the intersection
  EXPECT_EQ(intersection_type, LiteralType::Never);
}

// TEST_F(TypeContextTest, GetIntersectionOf_Interfaces) {
//   TypeId hashable_type = MakeStruct("Hashable", StructDeclaration::Interface);
//   TypeId printable_type = MakeStruct("Printable", StructDeclaration::Interface);
//
//   TypeId intersection_type =
//       type_context.GetIntersectionOf({hashable_type, printable_type});
//   // An intersection of interfaces results in a new type `Hashable&Printable`
//   EXPECT_NE(intersection_type, hashable_type);
//   EXPECT_NE(intersection_type, printable_type);
//   EXPECT_NE(intersection_type, LiteralType::Never);
//
//   EXPECT_EQ(type_registry.GetNameFromTypeId(intersection_type),
//             "(Hashable&Printable)");
//
//   // Absorption Rule: (A | (A & B)) => A
//   EXPECT_EQ(type_context.GetUnionOf({intersection_type, hashable_type}),
//             hashable_type);
//   EXPECT_EQ(type_context.GetUnionOf({intersection_type, printable_type}),
//             printable_type);
//
//   // Flatten
//   TypeId foo_type =
//       MakeStruct("Foo", StructDeclaration::Structure, {"Hashable"});
//   // Foo & Printable => Never
//   EXPECT_EQ(type_context.GetIntersectionOf({foo_type, printable_type}),
//             LiteralType::Never);
//   // Foo & Hashable => Foo
//   EXPECT_EQ(type_context.GetIntersectionOf({foo_type, hashable_type}),
//             foo_type);
//   // Hashable & Foo => Foo
//   EXPECT_EQ(type_context.GetIntersectionOf({hashable_type, foo_type}),
//             foo_type);
//   // Foo & (Hashable & Printable) => Never
//   EXPECT_EQ(type_context.GetIntersectionOf({foo_type, intersection_type}),
//             LiteralType::Never);
//
//   TypeId bar_type = MakeStruct("Bar", StructDeclaration::Structure,
//                                {"Hashable", "Printable"});
//   // Bar & Printable => Bar
//   EXPECT_EQ(type_context.GetIntersectionOf({bar_type, printable_type}),
//             bar_type);
//   // Bar & Hashable => Bar
//   EXPECT_EQ(type_context.GetIntersectionOf({bar_type, hashable_type}),
//             bar_type);
//   // Bar & (Hashable & Printable) => Bar
//   EXPECT_EQ(type_context.GetIntersectionOf({bar_type, intersection_type}),
//             bar_type);
// }
//
// TEST_F(TypeContextTest, GetUnionOf_Interfaces) {
//   TypeId hashable_type = MakeStruct("Hashable", StructDeclaration::Interface);
//   TypeId printable_type = MakeStruct("Printable", StructDeclaration::Interface);
//
//   TypeId union_type = type_context.GetUnionOf({hashable_type, printable_type});
//   // A union of interfaces results in a new type `Hashable|Printable`
//   EXPECT_NE(union_type, hashable_type);
//   EXPECT_NE(union_type, printable_type);
//   EXPECT_NE(union_type, LiteralType::Never);
//
//   EXPECT_EQ(type_registry.GetNameFromTypeId(union_type),
//             "(Hashable|Printable)");
//
//   // Absorption Rule: (A & (A | B)) => A
//   EXPECT_EQ(type_context.GetIntersectionOf({union_type, hashable_type}),
//             hashable_type);
//   EXPECT_EQ(type_context.GetIntersectionOf({union_type, printable_type}),
//             printable_type);
//
//   // Flatten
//   TypeId foo_type =
//       MakeStruct("Foo", StructDeclaration::Structure, {"Hashable"});
//   // Foo | Printable => Foo | Printable
//   EXPECT_EQ(type_registry.GetNameFromTypeId(
//                 type_context.GetUnionOf({foo_type, printable_type})),
//             "(Printable|Foo)");
//   // Foo | Hashable => Hashable
//   EXPECT_EQ(type_context.GetUnionOf({foo_type, hashable_type}), hashable_type);
//   // Hashable | Foo => Hashable
//   EXPECT_EQ(type_context.GetUnionOf({hashable_type, foo_type}), hashable_type);
//   // Foo | (Hashable | Printable) => Hashable | Printable
//   EXPECT_EQ(type_context.GetUnionOf({foo_type, union_type}), union_type);
//
//   TypeId bar_type = MakeStruct("Bar", StructDeclaration::Structure,
//                                {"Hashable", "Printable"});
//   // Bar | Printable => Printable
//   EXPECT_EQ(type_context.GetUnionOf({bar_type, printable_type}),
//             printable_type);
//   // Bar | Hashable => Hashable
//   EXPECT_EQ(type_context.GetUnionOf({bar_type, hashable_type}), hashable_type);
//   // Bar | Hashable | Printable => Hashable | Printable
//   EXPECT_EQ(type_context.GetUnionOf({bar_type, union_type}), union_type);
// }

}  // namespace