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
#include <string>

#include "compiler/type_registry.h"
#include "compiler/types.h"

class Printer {
 public:
  explicit Printer(const TypeRegistry* type_registry)
      : type_registry_(type_registry) {}

  void Print(const Block& block);
  void Print(const Statement& stmt, size_t indent = 0);
  void Print(const Expression& expr, size_t indent = 0);
  void Print(const FunctionDeclaration& fn, size_t indent = 0);

 private:
  const TypeRegistry* const type_registry_;

  void Print(const std::optional<ResolvedCall>& call, size_t indent = 0);

  std::string GetTypeName(std::optional<TypeId> type_id) const {
    return (type_registry_ && type_id)
               ? type_registry_->GetNameFromTypeId(*type_id)
               : "<unknown>";
  }
};