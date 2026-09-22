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
  using Bindings = std::unordered_map<SlotId, SpannedType>;

  explicit TypeResolver(TypeRegistry& type_registry,
                        TypeContext& type_context,
                        ErrorCollector& error_collector);
  ~TypeResolver();

  // Performs pattern matching on `concrete_type` using `pattern_type` to infer
  // the types of PlaceholderType(s) in `pattern_type`. If an error occurs due
  // to a mismatch between `concrete_type` and `pattern_type` then false is
  // returned. Any bindings that were determined will be added to `bindings`.
  // If a conflicting Type is resolved for an existing template parameter in
  // `bindings` then false will be returned (and an error logged).
  // Array[Box[i32]] + Array[T] => { T: Box[i32] }
  bool Resolve(TypeId pattern_type,
               TypeId concrete_type,
               Metadata resolution_span);

  std::optional<TypeId> NewPlaceholderTemplateOf(const NamedBinding& binding);

  TypeId Prune(TypeId type_id);

  std::string ToString() const;

  TypeId Rewrite(TypeId type_id);

  TypeId NewPlaceholder(TypeId type_id) {
    template_variables_.push_back(type_id);
    return type_registry_.NewPlaceholderType(next_placeholder_idx_++);
  }

 private:
  TypeRegistry& type_registry_;
  TypeContext& type_context_;
  ErrorCollector& error_collector_;

  Bindings bindings_;
  size_t next_placeholder_idx_{0};
  std::vector<TypeId> template_variables_;
};