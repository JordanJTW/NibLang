// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "compiler/error_collector.h"
#include "compiler/type_context.h"
#include "compiler/type_registry.h"
#include "compiler/types.h"

class TypeResolver {
 public:
  using Bindings = std::unordered_map<SlotId, TypeId>;

  explicit TypeResolver(TypeRegistry& type_registry,
                        TypeContext& type_context,
                        ErrorCollector& error_collector);

  // Performs pattern matching on `concrete_type` using `pattern_type` to infer
  // the types of PlaceholderType(s) in `pattern_type`. If an error occurs due
  // to a mismatch between `concrete_type` and `pattern_type` then false is
  // returned. Any bindings that were determined will be added to `bindings`.
  // If a conflicting Type is resolved for an existing template parameter in
  // `bindings` then false will be returned (and an error logged).
  // Array[Box[i32]] + Array[T] => { T: Box[i32] }
  bool Resolve(TypeId pattern_type, TypeId concrete_type, Bindings& bindings);

  // Attempts to deduce all required types to instantiate `binding` (which
  // represents a struct or function) given the `argument_types` used at a call-
  // site. Returns true if all required types were successfully deduced and the
  // `binding` can be instantiated by `bindings`, false otherwise.
  // `expression_metadata` is used to log errors in deducing a given variable.
  using CallArguments = std::vector<std::optional<SpannedType>>;
  bool Resolve(NamedBinding binding,
               const CallArguments& argument_types,
               std::vector<TypeId>& bindings,
               Metadata expression_metadata);

 private:
  TypeRegistry& type_registry_;
  TypeContext& type_context_;
  ErrorCollector& error_collector_;
};