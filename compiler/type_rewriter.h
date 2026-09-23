// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <unordered_map>

#include "compiler/type_context.h"
#include "compiler/type_registry.h"

using SubstitutionMap = std::unordered_map<TypeId, TypeId>;

class TypeRewriter {
 public:
  explicit TypeRewriter(TypeRegistry& type_registry, TypeContext& type_context);

  // Walks the type `type_id` and substitutes any types found within its tree
  // with new types from `substitutions` (if applicable).
  TypeId Rewrite(TypeId type_id, const SubstitutionMap& substitutions);

 private:
  TypeRegistry& type_registry_;
  TypeContext& type_context_;
};