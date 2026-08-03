#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

using SymbolId = size_t;
using ScopeId = size_t;
using TypeId = size_t;

struct FunctionDeclaration;
struct StructDeclaration;

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
  // The lexical environment this symbol was declared in
  ScopeId environment_scope_id;

  std::unordered_map<std::string, TypeId> default_template_type_ids;
  InstanceCache instances;

  inline bool IsExtern() const {
    if (parent_declaration.has_value())
      return parent_declaration.value()->is_extern && !declaration.body;

    return declaration.function_kind == FunctionKind::Extern;
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
  // The environment created within the Struct declaration
  ScopeId self_scope_id;
  std::vector<SymbolId> method_symbols;

  InstanceCache instances;
};