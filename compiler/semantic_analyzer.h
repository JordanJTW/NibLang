// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <memory>
#include <optional>

#include "compiler/error_collector.h"
#include "compiler/expression_checker.h"
#include "compiler/type_context.h"
#include "compiler/type_registry.h"
#include "compiler/types.h"

class SemanticAnalyzer {
 public:
  explicit SemanticAnalyzer(TypeContext& type_context,
                            ScopeManager& scope_manager,
                            ErrorCollector& error_collector,
                            TypeRegistry& type_registry);

  void Check(Block& block, FunctionContext& context);

 private:
  void CheckStatement(std::unique_ptr<Statement>& statement,
                      FunctionContext& context);

  TypeContext& type_context_;
  ScopeManager& scope_manager_;
  ErrorCollector& error_collector_;
  TypeRegistry& type_registry_;
};
