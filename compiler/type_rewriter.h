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