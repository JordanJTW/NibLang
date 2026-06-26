// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstddef>
#include <string>

#include "compiler/type_registry.h"
#include "compiler/types.h"

class Printer {
 public:
  Printer(const TypeRegistry* type_registry) : type_registry_(type_registry) {}

  void Print(const Block& block);
  void Print(const Statement& stmt, size_t indent = 0);
  void Print(const Expression& expr, size_t indent = 0);
  void Print(const FunctionDeclaration& fn, size_t indent = 0);

 private:
  const TypeRegistry* const type_registry_;

  std::string GetTypeName(TypeId type_id) const {
    return type_registry_ ? type_registry_->GetNameFromTypeId(type_id)
                          : "<unknown>";
  }
};