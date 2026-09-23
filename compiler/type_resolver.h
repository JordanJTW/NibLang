// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <unordered_map>

#include "compiler/error_collector.h"
#include "compiler/type_context.h"
#include "compiler/type_registry.h"
#include "compiler/types.h"

// Resolves placeholders $0, $1,... to concrete types through unification.
// Expects to live for the full duration of an expression chain collecting
// information through repeated calls to TypeResolver::Resolve.
class TypeResolver {
 public:
  explicit TypeResolver(TypeRegistry& type_registry,
                        TypeContext& type_context,
                        ErrorCollector& error_collector);
  ~TypeResolver();

  // Creates a new PlaceholderType ($0, $1,...) which is guaranteed to be unique
  // within the lifespan of a TypeResolver (i.e. a single expression chain).
  TypeId NewPlaceholder(TypeId type_id) {
    template_variables_.push_back(type_id);
    return type_registry_.NewPlaceholderType(next_placeholder_idx_++);
  }

  // Performs bidirectional unification on `concrete_type` and `pattern_type` to
  // infer $0, $1,... Returns `false` if types do not match structurally.
  bool Resolve(TypeId pattern_type,
               TypeId concrete_type,
               Metadata resolution_span);

  // Prunes chains of placeholders like $0 => $1 => i32 and updates them to
  // point to the concrete type i.e. $0 => i32, $1 => i32 returns the type.
  TypeId Prune(TypeId type_id);

  // Rewrites `type_id` to replace any placeholders in it with inferred types.
  TypeId Rewrite(TypeId type_id);

 private:
  using Bindings = std::unordered_map<SlotId, SpannedType>;

  TypeRegistry& type_registry_;
  TypeContext& type_context_;
  ErrorCollector& error_collector_;

  Bindings bindings_;
  size_t next_placeholder_idx_{0};
  std::vector<TypeId> template_variables_;
};
