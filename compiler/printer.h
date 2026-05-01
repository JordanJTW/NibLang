// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "compiler/type_context.h"
#include "compiler/types.h"

class Printer {
 public:
  Printer(TypeContext* type_context) : type_context_(type_context) {}

  void Print(const Block& block);
  void Print(const Statement& stmt, size_t indent = 0);
  void Print(const Expression& expr, size_t indent = 0);
  void Print(const FunctionDeclaration& fn, size_t indent = 0);

 private:
  TypeContext* const type_context_;

  std::string GetTypeName(TypeId type_id) const {
    return type_context_ ? type_context_->GetNameFromTypeId(type_id)
                         : "<unknown>";
  }
};