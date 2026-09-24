#include "compiler/type_registry.h"

#include "compiler/scope_manager.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

using LiteralType = TypeRegistry::LiteralType;

class TypeRegistryTest : public ::testing::Test {
 protected:
  ErrorCollector error_collector;
  ScopeManager scope_manager{error_collector};
  TypeRegistry type_registry{scope_manager};
};

TEST_F(TypeRegistryTest, BuiltInTypes) {
  // Ensure no gaps in type table that should cause a crash.
  for (size_t i = 0; i < TypeRegistry::kCount; ++i) {
    EXPECT_TRUE(type_registry.GetType<BuiltInType>(i));
  }
}

TEST_F(TypeRegistryTest, NewFunctionSymbol) {
  ScopeId current_scope = scope_manager.GetActiveScopeId();

  // Includes templates and a variadic as these should have no effect on Symbol
  FunctionDeclaration declaration = {
      SpannedText{"name"},
      /*arguments=*/{{SpannedText{"foo"}, ParsedType{"T"}}},
      /*return_type=*/ParsedType{"bool"},
      FunctionKind::Free,
      /*template_arguments=*/{{SpannedText{"T"}}},
      /*variadic_type=*/VariadicType{},
  };

  SymbolId symbol_id = type_registry.NewFunctionSymbol(declaration);

  const auto* const symbol = type_registry.GetSymbol<FunctionSymbol>(symbol_id);
  ASSERT_TRUE(symbol);

  EXPECT_EQ(symbol->instance_scope_id, current_scope);
  EXPECT_EQ(symbol->symbol_id, symbol_id);
}

TEST_F(TypeRegistryTest, NewFunctionType) {
  FunctionType type{/*arg_types=*/{LiteralType::Bool, LiteralType::i32},
                    /*return_type=*/LiteralType::Unit,
                    /*variadic_type=*/std::nullopt};

  TypeId type_id = type_registry.NewFunctionType(type);
  EXPECT_EQ(type_id, type_registry.NewFunctionType(type));

  type.variadic_type = LiteralType::i32;
  TypeId type_id_with_variadic = type_registry.NewFunctionType(type);
  EXPECT_NE(type_id_with_variadic, type_id);

  type.variadic_type = std::nullopt;
  EXPECT_EQ(type_id, type_registry.NewFunctionType(type));

  type.return_type = LiteralType::f32;
  TypeId type_id_with_return = type_registry.NewFunctionType(type);
  EXPECT_NE(type_id_with_return, type_id);

  type.arg_types.push_back(LiteralType::Bool);
  TypeId type_id_with_argc = type_registry.NewFunctionType(type);
  EXPECT_NE(type_id_with_argc, type_id_with_return);
}

TEST_F(TypeRegistryTest, NewOptionalType) {
  TypeId type_id = type_registry.NewTypeId();

  TypeId optional_type_id = type_registry.NewOptionalType(type_id);

  EXPECT_NE(type_id, optional_type_id);  // T? should be distinct from T
  // An optional T should always result in the same interned TypeId
  EXPECT_EQ(optional_type_id, type_registry.NewOptionalType(type_id));
  EXPECT_EQ(optional_type_id, type_registry.NewOptionalType(type_id));
}

TEST_F(TypeRegistryTest, GetNameFromTypeId_BuiltIns) {
  EXPECT_EQ(type_registry.GetNameFromTypeId(LiteralType::Unit), "'Unit'");
  EXPECT_EQ(type_registry.GetNameFromTypeId(LiteralType::i32), "'i32'");
  EXPECT_EQ(type_registry.GetNameFromTypeId(LiteralType::f32), "'f32'");
  EXPECT_EQ(type_registry.GetNameFromTypeId(LiteralType::Codepoint),
            "'Codepoint'");
  EXPECT_EQ(type_registry.GetNameFromTypeId(LiteralType::Bool), "'bool'");
  EXPECT_EQ(type_registry.GetNameFromTypeId(LiteralType::Any), "'any'");
  EXPECT_EQ(type_registry.GetNameFromTypeId(LiteralType::Never), "'never'");
  // Nil is special (an intrinsic type for a value) and should never be a part
  // of a Type definition, but it is convenient to still allow printing it.
  EXPECT_EQ(type_registry.GetNameFromTypeId(LiteralType::Nil), "'Nil'");
}

TEST_F(TypeRegistryTest, GetNameFromTypeId_FunctionType) {
  FunctionType type{/*arg_types=*/{LiteralType::i32, LiteralType::Bool},
                    /*return_type=*/LiteralType::f32,
                    /*variadic_type=*/std::nullopt};

  TypeId type_id = type_registry.NewFunctionType(std::move(type));
  EXPECT_EQ(type_registry.GetNameFromTypeId(type_id),
            "'fn (i32, bool) -> f32'");
}

TEST_F(TypeRegistryTest, GetNameFromTypeId_FunctionTypeWithVariadic) {
  FunctionType type{/*arg_types=*/{LiteralType::i32, LiteralType::Bool},
                    /*return_type=*/LiteralType::f32,
                    /*variadic_type=*/LiteralType::i32};

  TypeId type_id = type_registry.NewFunctionType(std::move(type));
  EXPECT_EQ(type_registry.GetNameFromTypeId(type_id),
            "'fn (i32, bool, ...i32) -> f32'");
}

// TEST_F(TypeRegistryTest, GetNameFromTypeId_StructType_Production) {
//   StructDeclaration declaration = {SpannedText{"Foo"},
//                                    /*template_arguments=*/{},
//                                    /*fields=*/{},
//                                    /*methods=*/{},
//                                    /*interfaces=*/{},
//                                    /*kind=*/StructDeclaration::Structure};
//   StructType type{declaration};
//
//   TypeId type_id = type_registry.NewTypeId();
//   type_registry.NewStructType(std::move(type), type_id);
//
//   EXPECT_EQ(type_registry.GetNameFromTypeId(type_id), "'Foo'");
// }
//
// TEST_F(TypeRegistryTest, GetNameFromTypeId_StructType_Debug) {
//   StructDeclaration declaration = {SpannedText{"Foo"},
//                                    /*template_arguments=*/{},
//                                    /*fields=*/{},
//                                    /*methods=*/{},
//                                    /*interfaces=*/{},
//                                    /*kind=*/StructDeclaration::Structure};
//   StructType type{declaration};
//
//   TypeId type_id = type_registry.NewTypeId();
//   type_registry.NewStructType(std::move(type), type_id);
//
//   TypeRegistry::FormatOptions options{.use_debug_names = true};
//   EXPECT_EQ(type_registry.GetNameFromTypeId(type_id, options), "struct Foo");
// }

// TEST_F(TypeRegistryTest, GetNameFromTypeId_StructTypeWithTemplate_Production) {
//   StructDeclaration declaration = {SpannedText{"Foo"},
//                                    /*template_arguments=*/{},
//                                    /*fields=*/{},
//                                    /*methods=*/{},
//                                    /*interfaces=*/{},
//                                    /*kind=*/StructDeclaration::Structure};
//   StructType type{declaration,
//                   /*field_types=*/{},
//                   /*template_arguments=*/{LiteralType::i32, LiteralType::Bool},
//                   /*interface_types=*/{},
//                   /*interface_scopes=*/{},
//                   /*scope_id*/ 0};
//
//   TypeId type_id = type_registry.NewTypeId();
//   type_registry.NewStructType(type, type_id);
//
//   EXPECT_EQ(type_registry.GetNameFromTypeId(type_id), "'Foo[i32, bool]'");
// }
//
// TEST_F(TypeRegistryTest, GetNameFromTypeId_StructTypeWithTemplate_Debug) {
//   StructDeclaration declaration = {SpannedText{"Foo"},
//                                    /*template_arguments=*/{},
//                                    /*fields=*/{},
//                                    /*methods=*/{},
//                                    /*interfaces=*/{},
//                                    /*kind=*/StructDeclaration::Interface};
//   StructType type{declaration,
//                   /*field_types=*/{},
//                   /*template_arguments=*/{LiteralType::i32, LiteralType::Bool},
//                   /*interface_types=*/{},
//                   /*interface_scopes=*/{},
//                   /*scope_id*/ 0};
//
//   TypeId type_id = type_registry.NewTypeId();
//   type_registry.NewStructType(type, type_id);
//
//   TypeRegistry::FormatOptions options{.use_debug_names = true};
//   EXPECT_EQ(type_registry.GetNameFromTypeId(type_id, options),
//             "interface Foo[i32, bool]");
// }

TEST_F(TypeRegistryTest, GetNameFromTypeId_UnionType_Production) {
  UnionType type{
      /*names=*/{LiteralType::Bool, LiteralType::i32, LiteralType::Codepoint}};

  TypeId type_id = type_registry.NewUnionType(std::move(type));

  EXPECT_EQ(type_registry.GetNameFromTypeId(type_id), "(bool|i32|Codepoint)");
}


TEST_F(TypeRegistryTest, GetNameFromTypeId_IntersectionType_Production) {
  IntersectionType type{
    /*names=*/{LiteralType::Bool, LiteralType::i32, LiteralType::Codepoint}};

  TypeId type_id = type_registry.NewIntersectionType(std::move(type));

  EXPECT_EQ(type_registry.GetNameFromTypeId(type_id), "(bool&i32&Codepoint)");
}

TEST_F(TypeRegistryTest, GetNameFromTypeId_OptionalType) {
  TypeId type_id = type_registry.NewOptionalType(LiteralType::Codepoint);

  EXPECT_EQ(type_registry.GetNameFromTypeId(type_id), "'Codepoint?'");
}

TEST_F(TypeRegistryTest, GetNameFromTypeId_AliasType_Production) {
  TypeId self_id = type_registry.NewTypeId();
  type_registry.NewAliasType("foo", self_id,
                             /*target_id=*/LiteralType::Codepoint);

  EXPECT_EQ(type_registry.GetNameFromTypeId(self_id), "alias 'foo'");
}

TEST_F(TypeRegistryTest, GetNameFromTypeId_AliasType_Debug) {
  TypeId self_id = type_registry.NewTypeId();
  type_registry.NewAliasType("foo", self_id,
                             /*target_id=*/LiteralType::Codepoint);

  TypeRegistry::FormatOptions options{.use_debug_names = true};
  EXPECT_EQ(type_registry.GetNameFromTypeId(self_id, options), "Alias[foo]");
}

TEST_F(TypeRegistryTest, GetNameFromTypeId_Unknown) {
  TypeId unassigned_id = type_registry.NewTypeId();
  EXPECT_EQ(type_registry.GetNameFromTypeId(unassigned_id), "'Unknown'");
}

TEST_F(TypeRegistryTest, FunctionType_OperatorEqual) {
  FunctionType ft1{
      {LiteralType::i32, LiteralType::f32}, LiteralType::Bool, false};
  FunctionType ft2{
      {LiteralType::i32, LiteralType::f32}, LiteralType::Bool, false};
  FunctionType ft3{{LiteralType::i32}, LiteralType::Bool, false};

  EXPECT_TRUE(ft1 == ft2);
  EXPECT_FALSE(ft1 == ft3);
}

TEST_F(TypeRegistryTest, UnionType_OperatorEqual) {
  UnionType ut1{{LiteralType::i32, LiteralType::f32}};
  UnionType ut2{{LiteralType::i32, LiteralType::f32}};
  UnionType ut3{{LiteralType::i32, LiteralType::Bool}};

  EXPECT_TRUE(ut1 == ut2);
  EXPECT_FALSE(ut1 == ut3);
}

TEST_F(TypeRegistryTest, FunctionType_Hash) {
  FunctionType::Hash hasher;
  FunctionType ft1{{LiteralType::i32}, LiteralType::Bool, false};
  FunctionType ft2{{LiteralType::i32}, LiteralType::Bool, false};
  FunctionType ft3{{LiteralType::f32}, LiteralType::Bool, false};
  FunctionType ft4{{LiteralType::i32}, LiteralType::Bool, true};

  EXPECT_EQ(hasher(ft1), hasher(ft2));
  EXPECT_NE(hasher(ft1), hasher(ft3));
  EXPECT_NE(hasher(ft1), hasher(ft4));
}

TEST_F(TypeRegistryTest, UnionType_Hash) {
  UnionType::Hash hasher;
  UnionType ut1{{LiteralType::i32, LiteralType::f32}};
  UnionType ut2{{LiteralType::i32, LiteralType::f32}};
  UnionType ut3{{LiteralType::i32, LiteralType::Bool}};

  EXPECT_EQ(hasher(ut1), hasher(ut2));
  EXPECT_NE(hasher(ut1), hasher(ut3));
}

}  // namespace
