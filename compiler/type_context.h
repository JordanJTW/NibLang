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
#include "compiler/scope_manager.h"
#include "compiler/types.h"

class ErrorCollector;

struct FunctionType {
  std::vector<TypeId> arg_types;
  TypeId return_type;
  bool is_variadic{false};

  // Represents the lexical scope of the function definition.
  ScopeId scope_id;

  bool operator==(const FunctionType& other) const;

  struct Hash {
    size_t operator()(const FunctionType& key) const;
  };
};

struct StructType {
  // TypeResolutionState resolution_state;
  const StructDeclaration& declaration;

  // A separate _ordered_ list of field types used for constructors.
  std::vector<TypeId> field_types;
  std::vector<TypeId> template_arguments;
  // Represents the lexical scope of the struct definition.
  ScopeId scope_id;
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
class TypeContext {
 public:
  enum LiteralType : TypeId {
    Void = 0,
    i32,
    f32,
    Codepoint,
    Bool,
    Any,
    Never,
    Nil,
    kCount
  };

  explicit TypeContext(ScopeManager& scope_manager);

  // Returns the FunctionDeclaration for the last function scope visited.
  FunctionDeclaration& GetCurrentFunction();

  // Creates a new StructSymbol for `declaration` in the symbol table and adds a
  // NamedBinding to it in the current scope. If the `declaration` is concrete
  // then a TypeId is assigned proactively (to be passed to DefineStructType).
  NamedBinding DeclareStructSymbol(StructDeclaration& declaration);
  // Performs type checking of the members/fields and creates a new StructType
  // for `self_id` from the results. `template_arguments` are stored on the new
  // StructType but are NOT used for any sort of template resolution here.
  void DefineStructType(TypeId self_id,
                        StructSymbol& symbol,
                        std::vector<TypeId> template_arguments,
                        ErrorCollector& error_collector);

  // Creates a new FunctionSymbol for `delcaration` in the symbol table and adds
  // a NamedBinding to it in the current scope. If `qualified_name` is passed it
  // will be passed to the symbol but does NOT effect the binding.
  SymbolId DeclareFunctionSymbol(
      FunctionDeclaration& declaration,
      std::optional<StructDeclaration*> parent_declaration = std::nullopt);

  // Declares a new function symbol in the current scope. Functions are
  // structurally typed based on signature. If this functions signature has not
  // been seen before a new TypeId will be created for it. Performs type
  // checking on `decl` and, if successful, returns the declared NamedBinding.
  // Errors are logged from within this function. `self_struct` and `self_id`
  // MUST be provided for method declarations.
  std::optional<NamedBinding> DefineFunction(
      SymbolId symbol_id,
      ErrorCollector* error_collector,
      std::optional<TypeId> self_id = std::nullopt);

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
    return it != type_lookup_.end() ? std::get_if<T>(&it->second) : nullptr;
  }

  // Returns a human readable representation of an interned `type_id`.
  std::string GetNameFromTypeId(TypeId type_id) const;

  std::optional<TypeId> GetTemplateOf(
      NamedBinding binding,
      const std::vector<TypeId>& argument_type_ids,
      ErrorCollector& error_collector);

  ParsedType GetParsedTypeFromId(TypeId) const;

  template <typename T>
  T* GetSymbol(SymbolId id) {
    auto it = symbol_table_.find(id);
    return it != symbol_table_.end() ? std::get_if<T>(&it->second) : nullptr;
  }

  template <typename T>
  const T* GetSymbol(SymbolId id) const {
    return const_cast<TypeContext*>(this)->GetSymbol<T>(id);
  }

  struct RealizedFunction {
    ScopeId scope_id;
    const FunctionDeclaration& delcaration;
    TypeId return_type_id;
  };

  std::vector<RealizedFunction> GetRealizedFunctions() {
    std::vector<RealizedFunction> current_functions = realized_functions_;
    realized_functions_.clear();
    return current_functions;
  }

  const auto& symbol_table() const { return symbol_table_; }

 private:
  ScopeManager& scope_manager_;

  CallIdx next_call_idx_ = 0;  // 0 is assigned to "main" by default.
  std::unordered_map<std::string, CallIdx> assigned_call_idx_;

  std::optional<TypeId> DeclareFunctionType(
      FunctionDeclaration& decl,
      ErrorCollector* error_collector,
      std::optional<TypeId> self_id = std::nullopt);

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

  std::unordered_map<SymbolId, std::variant<FunctionSymbol, StructSymbol>>
      symbol_table_;
  SymbolId next_symbol_id_{0};

  std::vector<RealizedFunction> realized_functions_;
};