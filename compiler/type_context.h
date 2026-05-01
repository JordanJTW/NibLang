// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "compiler/error_collector.h"
#include "compiler/logging.h"
#include "compiler/types.h"

class ErrorCollector;

struct FunctionType {
  std::vector<TypeId> arg_types;
  TypeId return_type;
  bool is_variadic{false};

  bool operator==(const FunctionType& other) const;

  struct Hash {
    size_t operator()(const FunctionType& key) const;
  };
};

struct StructType {
  // A separate _ordered_ list of field types used for constructors.
  std::vector<TypeId> field_types;
  // Allows looking up fields/methods Symbol by name.
  std::unordered_map<std::string, Symbol> member_symbols;
  // Structs are nominally typed, so type == Symbolic information.
  const StructDeclaration* struct_declaration;
};

struct UnionType {
  std::vector<TypeId> types;

  bool operator==(const UnionType other) const;

  struct Hash {
    size_t operator()(const UnionType& key) const;
  };
};

struct OptionalType {
  TypeId wrapped_type;
};

// Stores context for structural and nominal typing through-out the compiler.
// Lowers identifiers to Symbols with unique indexes per SymbolKind.
class TypeContext {
 public:
  enum LiteralType : TypeId {
    Void = 0,
    i32,
    f32,
    Codepoint,
    Bool,
    Any,
    Nil,
    kCount
  };

  explicit TypeContext(
      std::span<const std::string_view> external_functions = {});

  // Symbols are associated within the context of a function or block scope,
  // which allows for correct handling of variable shadowing, captures, etc.
  enum ScopeType { FunctionScope, BlockScope };
  // Enters a new scope of the given type. Function scopes are associated with
  // their FunctionDeclaration to allow for return type checking, etc.
  void EnterScope(ScopeType type, FunctionDeclaration* fn = nullptr);
  // Exits the current scope and returns to the parent scope.
  void ExitScope();

  // Returns the FunctionDeclaration for the last function scope visited.
  FunctionDeclaration& GetCurrentFunction();

  // Declares a new struct symbol in the current scope. Since structs are
  // nominally typed, this will create a new TypeId as well. This is split from
  // `DefineStructType` to allow for forward references to the struct type
  // within its own declaration (e.g. for recursive types).
  Symbol DeclareStructSymbol(std::string_view name);
  // Defines StructType for `self_id` by type checking `decl` and registering
  // field/member types. Returns false if any errors were encountered.
  void DefineStructType(TypeId self_id,
                        StructDeclaration& decl,
                        ErrorCollector& error_collector);

  // Declares a new function symbol in the current scope. Functions are
  // structurally typed based on signature. If this functions signature has not
  // been seen before a new TypeId will be created for it. Performs type
  // checking on `decl` and, if successful, returns the declared Symbol. Errors
  // are logged from within this function. `self_struct` and `self_id` MUST be
  // provided for method declarations.
  std::optional<Symbol> DefineFunction(
      FunctionDeclaration& decl,
      ErrorCollector* error_collector,
      std::optional<StructDeclaration*> self_struct = std::nullopt,
      std::optional<TypeId> self_id = std::nullopt);

  // Declares a Symbol of type VARIABLE in the current scope and returns it.
  Symbol DeclareVariableSymbol(std::string_view name, TypeId type_id);
  // Declares a Symbol of type CAPTURE in the current scope and returns it.
  Symbol DeclareCaptureSymbol(std::string_view name, TypeId type_id);
  // Declares a Symbol of type NARROWED in the current scope and returns it.
  Symbol DeclareNarrowedSymbol(Symbol symbol_to_narrow, TypeId narrowed_type);

  // Looks up a symbol by name within the given scope. By default, searches the
  // current scope and all parent scopes. If `scope` is Function, it will search
  // all parent scopes up to the nearest FunctionScope. If `scope` is Current,
  // it will only search the current scope.
  enum ScopeToCheck { Current, Function, Closure, All };
  std::optional<Symbol> GetSymbolFor(std::string_view name,
                                     ScopeToCheck scope = All) const;

  // Returns the TypeId for a given ParsedType if it can be resolved.
  std::optional<TypeId> GetTypeIdFor(const ParsedType& type);

  // Wraps a TypeId as an optional type, creating a new TypeId if needed.
  TypeId WrapTypeIdAsOptional(TypeId type_id);
  // Unwraps an optional TypeId to get the wrapped TypeId, returns std::nullopt
  // if the given TypeId is not an optional type.
  std::optional<TypeId> UnwrapOptionalTypeId(TypeId type_id) const;

  // Returns the TypeId for the union of `types`.
  TypeId GetUnionOf(const std::vector<TypeId>& types);

  // Returns if TypeId if Nil or could be Nil i.e. Nil + Optional.
  bool IsTypeNilable(TypeId type_id) const;

  // Returns true if `sub_type_id` is a subset of `super_type_id` (i.e. can be
  // used in its place). This is used for function argument type checking, etc.
  bool IsTypeSubsetOf(TypeId sub_type, TypeId super_type);

  template <typename T>
  const T* GetTypeInfo(TypeId type_id) const {
    auto it = type_lookup_.find(type_id);
    CHECK(it != type_lookup_.end())
        << "TypeInfo not registered for TypeId: " << type_id;
    return std::get_if<T>(&it->second);
  }

  const FunctionDeclaration& GetFunctionDeclaration(Symbol::Idx fn_idx) const {
    auto it = function_lookup_.find(fn_idx);
    CHECK(it != function_lookup_.end())
        << "FunctionDeclaration not registered for index: " << fn_idx;
    return *it->second;
  }

  // Returns a human readable representation of an interned `type_id`.
  std::string GetNameFromTypeId(TypeId type_id) const;

 private:
  enum class CreateIfMissing { YES, NO };
  std::optional<CallIdx> GetCallIdxFor(const std::string& name,
                                       CreateIfMissing create);

  CallIdx next_call_idx_ = 0;  // 0 is assigned to "main" by default.

  std::optional<TypeId> DeclareFunctionType(
      const FunctionDeclaration& decl,
      ErrorCollector* error_collector,
      std::optional<TypeId> self_id = std::nullopt);

  Symbol InsertSymbol(std::string_view name,
                      Symbol::Kind kind,
                      TypeId type_id,
                      std::optional<Symbol::Idx> idx = std::nullopt);

  struct Scope {
    ScopeType scope_type;
    FunctionDeclaration* fn;

    std::unordered_map<std::string, Symbol> symbols;
    Symbol::Idx next_symbol_idx{0};  // All variables are indexed within scope
  };
  std::vector<Scope> scopes_;
  size_t current_fn_scope_idx_{0};  // index into `scopes_`

  TypeId next_type_id_{LiteralType::kCount};  // TypeIds start after built-ins

  std::unordered_map<FunctionType, TypeId, FunctionType::Hash>
      interned_fn_type_;
  std::unordered_map<UnionType, TypeId, UnionType::Hash> interned_union_type_;
  // Maps wrapped_type -> optional_type_id for quick lookup.
  std::unordered_map<TypeId, TypeId> interned_optional_type_;

  // Declaring an interned "type" for the built-ins simplifies `IsTypeSubsetOf`.
  struct BuiltInType {};

  using TypeInfo = std::
      variant<BuiltInType, FunctionType, OptionalType, StructType, UnionType>;
  std::unordered_map<TypeId, TypeInfo> type_lookup_;

  // Function declarations can be looked up by CallIdx since they are 1:1.
  std::unordered_map<Symbol::Idx, const FunctionDeclaration*> function_lookup_;

  std::vector<std::string> external_functions_;
};