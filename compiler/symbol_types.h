/*
 * Copyright (C) 2026, Jordan Werthman <jordanwerthman@gmail.com>.
 *
 * This software is dual-licensed. You may use this file under the terms of
 * the GNU General Public License v3.0 (GPL-3.0) OR under a commercial
 * license obtained directly from the copyright holder.
 *
 * See the LICENSE.md file in the project root for full licensing details.
 *
 * SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-Commercial
 */

#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "compiler/types.h"

using SymbolId = size_t;
using ScopeId = size_t;
using TypeId = size_t;

enum class IntrinsicId {
  Float32_EqualTo,
  Float32_CompareTo,
  Float32_Add,
  Float32_Subtract,
  Float32_Multiply,
  Float32_Divide,
  Int32_EqualTo,
  Int32_CompareTo,
  Int32_Add,
  Int32_Subtract,
  Int32_Multiply,
  Int32_Divide,
  Int32_Modulus,
  Int32_BitwiseAnd,
  Int32_BitwiseXor,
  String_Concat,
};

struct InstanceHash {
  std::size_t operator()(const std::vector<TypeId>& argument_type_ids) const {
    std::size_t seed = argument_type_ids.size();
    for (const auto& id : argument_type_ids) {
      seed ^= std::hash<TypeId>{}(id) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    return seed;
  }
};

using InstanceCache =
    std::unordered_map<std::vector<TypeId>, TypeId, InstanceHash>;

struct FunctionSymbol {
  FunctionDeclaration& declaration;
  const std::optional<const StructDeclaration*> parent_declaration;
  SymbolId symbol_id;
  std::string qualified_name;
  std::optional<IntrinsicId> intrinsic_id;
  std::vector<TypeId> template_variable_type_ids;

  // The lexical environment this symbol was declared in
  ScopeId instance_scope_id;

  // The concrete type representing this symbol without substitutions
  // i.e. `fn (i32) -> i32` (non-templated) or `fn (T, T) -> N`
  TypeId canonical_type_id;

  // Stores StructSymbol (the implementor) => MethodSymbol (implementation)
  std::unordered_map<SymbolId, SymbolId> implementations;

  inline bool IsExtern() const {
    if (parent_declaration.has_value())
      return parent_declaration.value()->IsOpaque() && !declaration.body;

    return declaration.function_kind == FunctionKind::Extern;
  }

  inline bool IsFunctionBodyRequired() const {
    return !(IsExtern() || RequiresVirtualDispatch());
  }

  inline bool RequiresVirtualDispatch() const {
    if (parent_declaration.has_value())
      return parent_declaration.value()->IsInterface();

    return false;
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