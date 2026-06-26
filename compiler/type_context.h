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
#include "compiler/type_registry.h"
#include "compiler/types.h"

// Stores context for structural and nominal typing through-out the compiler.
class TypeContext {
 public:
  explicit TypeContext(ScopeManager& scope_manager,
                       TypeRegistry& type_registry,
                       ErrorCollector& error_collector);

  // Performs type checking of the members/fields and creates a new StructType
  // for `self_id` from the results. `template_arguments` are stored on the new
  // StructType but are NOT used for any sort of template resolution here.
  void DefineStructType(TypeId self_id,
                        StructSymbol& symbol,
                        std::vector<TypeId> template_arguments);

  // Declares a new function symbol in the current scope. Functions are
  // structurally typed based on signature. If this functions signature has not
  // been seen before a new TypeId will be created for it. Performs type
  // checking on `decl` and, if successful, returns the declared NamedBinding.
  // Errors are logged from within this function. `self_struct` and `self_id`
  // MUST be provided for method declarations.
  std::optional<NamedBinding> DefineFunction(
      SymbolId symbol_id,
      std::optional<TypeId> self_id = std::nullopt);

  // Returns the TypeId for a given ParsedType if it can be resolved.
  std::optional<TypeId> GetTypeIdFor(const ParsedType& type);

  // If `type_id` is an Optional[T], returns T. Otherwise, returns std::nullopt.
  std::optional<TypeId> UnwrapOptional(TypeId type_id) const;

  // Wraps `type_id` as an Optional[T]. Returns the TypeId for Optional[T].
  TypeId GetOptionalOf(TypeId type_id);

  // Returns the TypeId for the union of `types`.
  TypeId GetUnionOf(const std::vector<TypeId>& types);

  // Creates an alias of `name` pointing to `type`.
  TypeId GetAliasOf(std::string_view name, const ParsedType& type);

  // Returns if TypeId if Nil or could be Nil i.e. Nil + Optional.
  bool IsTypeNilable(TypeId type_id) const;

  // Returns true if `sub_type_id` is a subset of `super_type_id` (i.e. can be
  // used in its place). This is used for function argument type checking, etc.
  bool IsTypeSubsetOf(TypeId sub_type, TypeId super_type) const;

  std::optional<TypeId> GetTemplateOf(
      NamedBinding binding,
      const std::vector<TypeId>& argument_type_ids);

  ParsedType GetParsedTypeFromId(TypeId) const;

  struct RealizedFunction {
    ScopeId scope_id;
    FunctionDeclaration& delcaration;
    TypeId return_type_id;
  };

  std::vector<RealizedFunction> GetRealizedFunctions() {
    std::vector<RealizedFunction> current_functions = realized_functions_;
    realized_functions_.clear();
    return current_functions;
  }

 private:
  friend std::ostream& operator<<(std::ostream&, const TypeContext&);

  ScopeManager& scope_manager_;
  TypeRegistry& type_registry_;
  ErrorCollector& error_collector_;

  std::optional<TypeInstance> DeclareFunctionType(
      FunctionDeclaration& decl,
      std::optional<TypeId> self_id = std::nullopt);

  std::vector<RealizedFunction> realized_functions_;
};

std::ostream& operator<<(std::ostream&, const TypeContext&);