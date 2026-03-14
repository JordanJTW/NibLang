#include "compiler/type_context.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "compiler/error_collector.h"
#include "compiler/tokenizer.h"
#include "compiler/types.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "src/vm.h"

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::Return;

namespace {

class TypeContextTest : public ::testing::Test {
 protected:
  std::unique_ptr<ErrorCollector> error_collector{
      DefaultErrorCollector("test_file")};
  TypeContext type_context{*error_collector};
};

TEST_F(TypeContextTest, GetTypeIdFor_BuiltInType) {
  static const std::unordered_map<std::string, TypeId> kBuiltInTypes = {
      {"Void", TypeContext::Void}, {"i32", TypeContext::i32},
      {"f32", TypeContext::f32},   {"bool", TypeContext::Bool},
      {"any", TypeContext::Any},
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

  auto symbol = type_context.DefineFunction(fn_decl);
  ASSERT_TRUE(symbol.has_value());
  EXPECT_EQ(symbol->kind, Symbol::Function);

  // Check that the function type is registered
  auto fn_type = type_context.GetTypeInfo<FunctionType>(symbol->type_id);
  EXPECT_EQ(fn_type->arg_types.size(), 2);
  EXPECT_EQ(fn_type->arg_types[0], TypeContext::i32);
  EXPECT_EQ(fn_type->arg_types[1], TypeContext::f32);
  EXPECT_EQ(fn_type->return_type, TypeContext::Bool);
  EXPECT_FALSE(fn_type->is_variadic);

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
  auto union_info = type_context.GetTypeInfo<UnionType>(type_id.value());
  EXPECT_EQ(union_info->types.size(), 2);
  EXPECT_TRUE(std::find(union_info->types.begin(), union_info->types.end(),
                        TypeContext::i32) != union_info->types.end());
  EXPECT_TRUE(std::find(union_info->types.begin(), union_info->types.end(),
                        TypeContext::f32) != union_info->types.end());
}

TEST_F(TypeContextTest, GetTypeIdFor_FunctionType) {
  ParsedFunctionType fn_type;
  fn_type.arguments = {ParsedType{"i32"}, ParsedType{"f32"}};
  fn_type.return_value = std::make_shared<ParsedType>(ParsedType{"bool"});
  auto type_id = type_context.GetTypeIdFor(ParsedType{fn_type});
  ASSERT_TRUE(type_id.has_value());
  auto fn_info = type_context.GetTypeInfo<FunctionType>(type_id.value());
  EXPECT_EQ(fn_info->arg_types.size(), 2);
  EXPECT_EQ(fn_info->arg_types[0], TypeContext::i32);
  EXPECT_EQ(fn_info->arg_types[1], TypeContext::f32);
  EXPECT_EQ(fn_info->return_type, TypeContext::Bool);
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
  auto fn_info = type_context.GetTypeInfo<FunctionType>(type_id.value());
  EXPECT_EQ(fn_info->return_type, TypeContext::Void);
}

TEST_F(TypeContextTest, DeclareVariableSymbol) {
  auto symbol =
      type_context.DeclareVariableSymbol("test_var", TypeContext::i32);
  EXPECT_EQ(symbol.kind, Symbol::Variable);
  EXPECT_EQ(symbol.type_id, TypeContext::i32);
  EXPECT_TRUE(symbol.idx.has_value());
}

TEST_F(TypeContextTest, DeclareCaptureSymbol) {
  auto symbol =
      type_context.DeclareCaptureSymbol("test_capture", TypeContext::f32);
  EXPECT_EQ(symbol.kind, Symbol::Capture);
  EXPECT_EQ(symbol.type_id, TypeContext::f32);
  EXPECT_TRUE(symbol.idx.has_value());
}

TEST_F(TypeContextTest, GetSymbolFor_Variable) {
  type_context.DeclareVariableSymbol("test_var", TypeContext::i32);
  auto symbol = type_context.GetSymbolFor("test_var");
  ASSERT_TRUE(symbol.has_value());
  EXPECT_EQ(symbol->kind, Symbol::Variable);
  EXPECT_EQ(symbol->type_id, TypeContext::i32);
}

TEST_F(TypeContextTest, GetSymbolFor_NotFound) {
  auto symbol = type_context.GetSymbolFor("nonexistent");
  EXPECT_FALSE(symbol.has_value());
}

TEST_F(TypeContextTest, DeclareStructSymbol) {
  auto symbol = type_context.DeclareStructSymbol("TestStruct");
  EXPECT_GT(symbol.type_id, TypeContext::LiteralType::kCount);
  EXPECT_EQ(symbol.kind, Symbol::Struct);
  EXPECT_FALSE(symbol.idx.has_value());  // Structs have no index
}

TEST_F(TypeContextTest, DefineStructType) {
  StructDeclaration struct_decl;
  struct_decl.name = "TestStruct";
  struct_decl.fields = {{"field1", ParsedType{"i32"}},
                        {"field2", ParsedType{"f32"}}};
  struct_decl.is_extern = false;

  auto struct_symbol = type_context.DeclareStructSymbol("TestStruct");
  type_context.DefineStructType(struct_symbol.type_id, struct_decl);

  // Check that struct type is registered
  auto struct_info =
      type_context.GetTypeInfo<StructType>(struct_symbol.type_id);
  EXPECT_EQ(struct_info->struct_declaration->name, "TestStruct");
  EXPECT_EQ(struct_info->field_types.size(), 2);
  EXPECT_EQ(struct_info->field_types[0], TypeContext::i32);
  EXPECT_EQ(struct_info->field_types[1], TypeContext::f32);
  EXPECT_EQ(struct_info->member_symbols.size(), 2);
  ASSERT_TRUE(struct_info->member_symbols.count("field1"));
  ASSERT_TRUE(struct_info->member_symbols.count("field2"));
  EXPECT_EQ(struct_info->member_symbols.at("field1"),
            (Symbol{Symbol::Field, TypeContext::i32, /*idx=*/0}));
  EXPECT_EQ(struct_info->member_symbols.at("field2"),
            (Symbol{Symbol::Field, TypeContext::f32, /*idx=*/1}));
}

TEST_F(TypeContextTest, DefineFunction_Method) {
  StructDeclaration struct_decl;
  struct_decl.name = "TestStruct";
  struct_decl.is_extern = false;

  auto struct_symbol = type_context.DeclareStructSymbol("TestStruct");
  type_context.DefineStructType(struct_symbol.type_id, struct_decl);

  FunctionDeclaration method_decl{
      .name = "test_method",
      .arguments = {{"self", ParsedType{"TestStruct"}},
                    {"arg", ParsedType{"i32"}}},
      .return_type = ParsedType{"Void"},
      .function_kind = FunctionKind::Method,
      .body = std::make_unique<Block>(),
  };

  auto symbol = type_context.DefineFunction(method_decl, &struct_decl,
                                            struct_symbol.type_id);
  ASSERT_TRUE(symbol.has_value());
  EXPECT_EQ(symbol->kind, Symbol::Function);
}

TEST_F(TypeContextTest, DefineFunction_ExternMethod) {
  StructDeclaration struct_decl;
  struct_decl.name = "String";
  struct_decl.is_extern = true;

  auto struct_symbol = type_context.DeclareStructSymbol("String");
  type_context.DefineStructType(struct_symbol.type_id, struct_decl);

  FunctionDeclaration method_decl{
      .name = "length",
      .arguments = {{"self", ParsedType{"String"}}},
      .return_type = ParsedType{"i32"},
      .function_kind = FunctionKind::Method,
  };

  auto symbol = type_context.DefineFunction(method_decl, &struct_decl,
                                            struct_symbol.type_id);
  ASSERT_TRUE(symbol.has_value());
  EXPECT_EQ(symbol->kind, Symbol::Function);
  ASSERT_TRUE(symbol->idx.has_value());
  EXPECT_EQ(symbol->idx.value(), VM_BUILTIN_STRING_LENGTH);
}

TEST_F(TypeContextTest, DefineFunction_UnknownExtern) {
  FunctionDeclaration extern_fn{
      .name = "UnknownExternFunction",
      .arguments = {{"arg", ParsedType{"i32"}}},
      .return_type = ParsedType{"i32"},
      .function_kind = FunctionKind::Extern,
  };

  auto symbol = type_context.DefineFunction(extern_fn);
  EXPECT_FALSE(symbol.has_value());  // CallIdx can not be determined
}

TEST_F(TypeContextTest, IsTypeEquivalent) {
  EXPECT_TRUE(
      TypeContext::IsTypeEquivalent(TypeContext::i32, TypeContext::i32));
  EXPECT_TRUE(
      TypeContext::IsTypeEquivalent(TypeContext::f32, TypeContext::f32));
  EXPECT_TRUE(
      TypeContext::IsTypeEquivalent(TypeContext::i32, TypeContext::f32));
  EXPECT_TRUE(
      TypeContext::IsTypeEquivalent(TypeContext::f32, TypeContext::i32));
  EXPECT_FALSE(
      TypeContext::IsTypeEquivalent(TypeContext::i32, TypeContext::Bool));
  EXPECT_FALSE(
      TypeContext::IsTypeEquivalent(TypeContext::Void, TypeContext::Any));
}

TEST_F(TypeContextTest, IsTypeSubsetOf) {
  // Equivalent types
  EXPECT_TRUE(type_context.IsTypeSubsetOf(TypeContext::i32, TypeContext::i32));
  EXPECT_TRUE(type_context.IsTypeSubsetOf(
      TypeContext::i32, TypeContext::f32));  // numeric coercion

  // Any type
  EXPECT_TRUE(type_context.IsTypeSubsetOf(TypeContext::i32, TypeContext::Any));
  EXPECT_TRUE(type_context.IsTypeSubsetOf(TypeContext::Bool, TypeContext::Any));

  // Union types
  ParsedUnionType union_type;
  union_type.names = {ParsedType{"i32"}, ParsedType{"f32"}, ParsedType{"bool"}};
  auto union_id = type_context.GetTypeIdFor(ParsedType{union_type});
  ASSERT_TRUE(union_id.has_value());

  Symbol test_struct = type_context.DeclareStructSymbol("TestStruct");
  StructDeclaration test_struct_decl;
  test_struct_decl.is_extern = true;
  type_context.DefineStructType(test_struct.type_id, test_struct_decl);

  EXPECT_TRUE(type_context.IsTypeSubsetOf(TypeContext::i32, union_id.value()));
  EXPECT_TRUE(type_context.IsTypeSubsetOf(TypeContext::Bool, union_id.value()));
  EXPECT_FALSE(
      type_context.IsTypeSubsetOf(test_struct.type_id, union_id.value()));

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
      type_context.IsTypeSubsetOf(TypeContext::i32, new_union_id.value()));
  EXPECT_TRUE(
      type_context.IsTypeSubsetOf(test_struct.type_id, new_union_id.value()));
  EXPECT_FALSE(
      type_context.IsTypeSubsetOf(TypeContext::Bool, new_union_id.value()));
}

TEST_F(TypeContextTest, GetNameFromTypeId) {
  EXPECT_EQ(type_context.GetNameFromTypeId(TypeContext::Void), "Void");
  EXPECT_EQ(type_context.GetNameFromTypeId(TypeContext::i32), "i32");
  EXPECT_EQ(type_context.GetNameFromTypeId(TypeContext::f32), "f32");
  EXPECT_EQ(type_context.GetNameFromTypeId(TypeContext::Bool), "bool");
  EXPECT_EQ(type_context.GetNameFromTypeId(TypeContext::Any), "any");

  // Function type
  FunctionDeclaration fn_decl{
      .name = "test_fn",
      .arguments = {{"arg1", ParsedType{"i32"}}},
      .return_type = ParsedType{"bool"},
      .function_kind = FunctionKind::Free,
      .body = std::make_unique<Block>(),
  };
  auto symbol = type_context.DefineFunction(fn_decl);
  ASSERT_TRUE(symbol.has_value());
  std::string fn_name = type_context.GetNameFromTypeId(symbol->type_id);
  EXPECT_EQ(fn_name, "fn (i32) -> bool");

  // Variadic function
  FunctionDeclaration variadic_fn{
      .name = "variadic_fn",
      .arguments = {{"arg1", ParsedType{"i32"}}},
      .return_type = ParsedType{"bool"},
      .function_kind = FunctionKind::Free,
      .is_variadic = true,
      .body = std::make_unique<Block>(),
  };
  auto variadic_symbol = type_context.DefineFunction(variadic_fn);
  ASSERT_TRUE(variadic_symbol.has_value());
  std::string variadic_name =
      type_context.GetNameFromTypeId(variadic_symbol->type_id);
  EXPECT_EQ(variadic_name, "fn (i32, ...) -> bool");

  // Struct type
  StructDeclaration struct_decl;
  struct_decl.name = "TestStruct";
  struct_decl.is_extern = false;
  auto struct_symbol = type_context.DeclareStructSymbol("TestStruct");
  type_context.DefineStructType(struct_symbol.type_id, struct_decl);
  EXPECT_EQ(type_context.GetNameFromTypeId(struct_symbol.type_id),
            "struct TestStruct");

  // Union type
  ParsedUnionType union_type;
  union_type.names = {ParsedType{"i32"}, ParsedType{"f32"}, ParsedType{"i32"}};
  auto union_id = type_context.GetTypeIdFor(ParsedType{union_type});
  ASSERT_TRUE(union_id.has_value());
  std::string union_name = type_context.GetNameFromTypeId(union_id.value());
  EXPECT_EQ(union_name, "(i32, f32)");
}

TEST_F(TypeContextTest, GetFunctionDeclaration) {
  FunctionDeclaration fn_decl{
      .name = "test_fn",
      .arguments = {{"arg1", ParsedType{"i32"}}},
      .return_type = ParsedType{"bool"},
      .function_kind = FunctionKind::Free,
      .body = std::make_unique<Block>(),
  };
  auto symbol = type_context.DefineFunction(fn_decl);
  ASSERT_TRUE(symbol.has_value());
  ASSERT_TRUE(symbol->idx.has_value());

  const auto& retrieved_fn =
      type_context.GetFunctionDeclaration(symbol->idx.value());
  EXPECT_EQ(retrieved_fn.name, "test_fn");
  EXPECT_EQ(retrieved_fn.arguments.size(), 1);
  EXPECT_EQ(retrieved_fn.arguments[0].first, "arg1");
}

TEST_F(TypeContextTest, GetSymbolFor_ScopeChecks) {
  type_context.DeclareVariableSymbol("v1", TypeContext::i32);

  type_context.EnterScope(TypeContext::ScopeType::FunctionScope);
  type_context.DeclareVariableSymbol("v2", TypeContext::Bool);

  type_context.EnterScope(TypeContext::ScopeType::BlockScope);
  type_context.DeclareVariableSymbol("v3", TypeContext::f32);

  // (BlockScope) v3 accessible but not v1 or v2
  auto symbol_current =
      type_context.GetSymbolFor("v3", TypeContext::ScopeToCheck::Current);
  ASSERT_TRUE(symbol_current.has_value());
  EXPECT_EQ(symbol_current->type_id, TypeContext::f32);

  EXPECT_FALSE(
      type_context.GetSymbolFor("v1", TypeContext::ScopeToCheck::Current));
  EXPECT_FALSE(
      type_context.GetSymbolFor("v2", TypeContext::ScopeToCheck::Current));

  // (All) v1, v2, and v3 accessible
  auto v1_all = type_context.GetSymbolFor("v1", TypeContext::ScopeToCheck::All);
  ASSERT_TRUE(v1_all.has_value());
  EXPECT_EQ(v1_all->type_id, TypeContext::i32);

  auto v2_all = type_context.GetSymbolFor("v2", TypeContext::ScopeToCheck::All);
  ASSERT_TRUE(v2_all.has_value());
  EXPECT_EQ(v2_all->type_id, TypeContext::Bool);

  auto v3_all = type_context.GetSymbolFor("v3", TypeContext::ScopeToCheck::All);
  ASSERT_TRUE(v3_all.has_value());
  EXPECT_EQ(v3_all->type_id, TypeContext::f32);

  // (FunctionScope) v2 and v3 accessible but not v1
  auto v2_fn =
      type_context.GetSymbolFor("v2", TypeContext::ScopeToCheck::Function);
  ASSERT_TRUE(v2_fn.has_value());
  EXPECT_EQ(v2_fn->type_id, TypeContext::Bool);

  auto v3_fn =
      type_context.GetSymbolFor("v3", TypeContext::ScopeToCheck::Function);
  ASSERT_TRUE(v3_fn.has_value());
  EXPECT_EQ(v3_fn->type_id, TypeContext::f32);

  EXPECT_FALSE(
      type_context.GetSymbolFor("v1", TypeContext::ScopeToCheck::Function));

  type_context.ExitScope();  // Exit Block Scope

  EXPECT_FALSE(type_context.GetSymbolFor("v3", TypeContext::ScopeToCheck::All));

  type_context.ExitScope();  // Exit Function Scope

  EXPECT_FALSE(type_context.GetSymbolFor("v2", TypeContext::ScopeToCheck::All));
}

TEST_F(TypeContextTest, GetSymbolFor_Shadowing) {
  // Declare in current ("main") scope
  Symbol outer = type_context.DeclareVariableSymbol("var", TypeContext::i32);

  // Enter block scope
  type_context.EnterScope(TypeContext::ScopeType::BlockScope);
  Symbol inner = type_context.DeclareVariableSymbol("var", TypeContext::f32);

  // Should find inner symbol in current scope
  auto symbol_current =
      type_context.GetSymbolFor("var", TypeContext::ScopeToCheck::Current);
  ASSERT_TRUE(symbol_current.has_value());
  EXPECT_EQ(symbol_current->type_id, TypeContext::f32);

  // Should find inner symbol in all scopes (shadows outer)
  auto symbol_all =
      type_context.GetSymbolFor("var", TypeContext::ScopeToCheck::All);
  ASSERT_TRUE(symbol_all.has_value());
  EXPECT_EQ(symbol_all->type_id, TypeContext::f32);

  type_context.ExitScope();

  // After exiting, should find outer symbol again
  auto symbol_after_exit =
      type_context.GetSymbolFor("var", TypeContext::ScopeToCheck::All);
  ASSERT_TRUE(symbol_after_exit.has_value());
  EXPECT_EQ(symbol_after_exit->type_id, TypeContext::i32);
}

TEST_F(TypeContextTest, GetCurrentFunction) {
  // In global scope, GetCurrentFunction should return "main"
  auto& main_fn = type_context.GetCurrentFunction();
  EXPECT_EQ(main_fn.name, "main");
  EXPECT_EQ(main_fn.function_kind, FunctionKind::Free);

  // Define a new function and enter its scope
  FunctionDeclaration fn_decl{
      .name = "test_fn",
      .arguments = {},
      .return_type = ParsedType{"Void"},
      .function_kind = FunctionKind::Free,
      .body = std::make_unique<Block>(),
  };
  auto symbol = type_context.DefineFunction(fn_decl);
  ASSERT_TRUE(symbol.has_value());
  type_context.EnterScope(TypeContext::ScopeType::FunctionScope, &fn_decl);

  auto& current_fn = type_context.GetCurrentFunction();
  EXPECT_EQ(current_fn.name, "test_fn");
}

TEST_F(TypeContextTest, FunctionType_OperatorEqual) {
  FunctionType ft1{
      {TypeContext::i32, TypeContext::f32}, TypeContext::Bool, false};
  FunctionType ft2{
      {TypeContext::i32, TypeContext::f32}, TypeContext::Bool, false};
  FunctionType ft3{{TypeContext::i32}, TypeContext::Bool, false};

  EXPECT_TRUE(ft1 == ft2);
  EXPECT_FALSE(ft1 == ft3);
}

TEST_F(TypeContextTest, UnionType_OperatorEqual) {
  UnionType ut1{{TypeContext::i32, TypeContext::f32}};
  UnionType ut2{{TypeContext::i32, TypeContext::f32}};
  UnionType ut3{{TypeContext::i32, TypeContext::Bool}};

  EXPECT_TRUE(ut1 == ut2);
  EXPECT_FALSE(ut1 == ut3);
}

TEST_F(TypeContextTest, FunctionType_Hash) {
  FunctionType::Hash hasher;
  FunctionType ft1{{TypeContext::i32}, TypeContext::Bool, false};
  FunctionType ft2{{TypeContext::i32}, TypeContext::Bool, false};
  FunctionType ft3{{TypeContext::f32}, TypeContext::Bool, false};
  FunctionType ft4{{TypeContext::i32}, TypeContext::Bool, true};

  EXPECT_EQ(hasher(ft1), hasher(ft2));
  EXPECT_NE(hasher(ft1), hasher(ft3));
  EXPECT_NE(hasher(ft1), hasher(ft4));
}

TEST_F(TypeContextTest, UnionType_Hash) {
  UnionType::Hash hasher;
  UnionType ut1{{TypeContext::i32, TypeContext::f32}};
  UnionType ut2{{TypeContext::i32, TypeContext::f32}};
  UnionType ut3{{TypeContext::i32, TypeContext::Bool}};

  EXPECT_EQ(hasher(ut1), hasher(ut2));
  EXPECT_NE(hasher(ut1), hasher(ut3));
}

}  // namespace