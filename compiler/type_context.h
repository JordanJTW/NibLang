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

  // Whether a function (or struct's method) should have the body type-checked
  // or only the signature (only the signature is needed for type deduction).
  enum class CheckFunctionBody { YES, NO };

  // Returns the TypeId for a given ParsedType if it can be resolved.
  std::optional<TypeId> GetTypeIdFor(const ParsedType& type);

  // If `type_id` is an Optional[T], returns T. Otherwise, returns std::nullopt.
  std::optional<TypeId> UnwrapOptional(TypeId type_id) const;

  // Wraps `type_id` as an Optional[T]. Returns the TypeId for Optional[T].
  TypeId GetOptionalOf(TypeId type_id);

  // Returns the TypeId for the union of `types`.
  TypeId GetUnionOf(const std::vector<TypeId>& types);

  // Returns the TypeId for the intersection of `types`.
  TypeId GetIntersectionOf(const std::vector<TypeId>& types);

  // Realizes a template of a struct or function pointed to by `symbol_id` into
  // a concrete TypeId using `argument_type_ids` as template arguments.
  // `instantiation_span` is the expression/type annotations span for errors
  std::optional<TypeId> GetTemplateOf(
      SymbolId symbol_id,
      const std::vector<ParsedType>& argument_types,
      Metadata instantiation_span);

  // Returns if TypeId is Nil or could be Nil i.e. Nil + Optional.
  bool IsTypeNilable(TypeId type_id) const;

  // Returns true if `sub_type_id` is a subset of `super_type_id` (i.e. can be
  // used in its place). This is used for function argument type checking, etc.
  bool IsTypeSubsetOf(TypeId sub_type, TypeId super_type) const;

  // Returns true if `t1` and `t2` so not are completely unrelated.
  bool AreDisjointTypes(TypeId t1, TypeId t2) const;

  struct RealizedFunction {
    ScopeId scope_id;
    FunctionDeclaration& declaration;
    SpannedType return_type;
  };

  std::vector<RealizedFunction> GetRealizedFunctions() {
    std::vector<RealizedFunction> current_functions = realized_functions_;
    realized_functions_.clear();
    return current_functions;
  }

  std::optional<TypeInstance> DeclareFunctionType(
      FunctionSymbol& symbol,
      CheckFunctionBody check_fn_body,
      std::optional<TypeId> self_id = std::nullopt);

 private:
  friend std::ostream& operator<<(std::ostream&, const TypeContext&);

  ScopeManager& scope_manager_;
  TypeRegistry& type_registry_;
  ErrorCollector& error_collector_;

  // Consolidates subtypes in the set into their implemented base interfaces.
  void FlattenSubtypesUnion(std::set<TypeId>& types) const;

  // Simplifies the intersection type set by removing redundant base types.
  void FlattenSubtypesIntersection(std::set<TypeId>& types) const;

  std::vector<RealizedFunction> realized_functions_;
};

std::ostream& operator<<(std::ostream&, const TypeContext&);