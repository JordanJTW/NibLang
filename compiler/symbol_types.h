// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "compiler/types.h"

using SymbolId = size_t;
using ScopeId = size_t;
using TypeId = size_t;

struct InstanceHash {
  std::size_t operator()(const std::vector<TypeId>& argument_type_ids) const {
    std::size_t seed = argument_type_ids.size();
    for (const auto& id : argument_type_ids) {
      seed ^= std::hash<TypeId>{}(id) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    return seed;
  }
};

struct TypeInstance {
  TypeId type_id;
  ScopeId scope_id;
};

using InstanceCache =
    std::unordered_map<std::vector<TypeId>, TypeInstance, InstanceHash>;

struct FunctionSymbol {
  FunctionDeclaration& declaration;
  const std::optional<const StructDeclaration*> parent_declaration;
  SymbolId symbol_id;
  std::vector<TypeId> template_variable_type_ids;

  // The lexical environment this symbol was declared in
  ScopeId instance_scope_id;

  // The concrete type representing this symbol without substitutions
  // i.e. `fn (i32) -> i32` (non-templated) or `fn (T, T) -> N`
  TypeId canonical_type_id;

  // Stores StructSymbol (the implementor) => MethodSymbol (implementation)
  std::unordered_map<SymbolId, SymbolId> implementations;
  InstanceCache instances;

  inline bool IsExtern() const {
    if (parent_declaration.has_value())
      return parent_declaration.value()->IsOpaque() && !declaration.body;

    return declaration.function_kind == FunctionKind::Extern;
  }

  inline bool IsMethodBodyRequired() const {
    return !(IsExtern() || RequiresVirtualDispatch());
  }

  inline bool RequiresVirtualDispatch() const {
    if (parent_declaration.has_value())
      return parent_declaration.value()->IsInterface();

    return false;
  }

  inline std::string GetName() const {
    if (parent_declaration.has_value())
      return parent_declaration.value()->name.text + "_" +
             declaration.name.text;

    return declaration.name.text;
  }
};

struct StructSymbol {
  StructDeclaration& declaration;
  SymbolId symbol_id;
  // Namespace scope holding static members
  ScopeId static_scope_id;
  // Instance scope holding things that take `self`
  ScopeId instance_scope_id;

  std::vector<TypeId> template_variable_type_ids;

  // All the following are represented in the canonical form:
  // A separate _ordered_ list of field types used for constructors.
  std::vector<TypeId> field_types;
  // The interfaces implemented by this struct.
  std::vector<TypeId> interface_types;
  std::vector<ScopeId> interface_scopes;
};