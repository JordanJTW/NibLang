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

#include <memory>
#include <optional>
#include <vector>

#include "compiler/error_collector.h"
#include "compiler/expression_checker.h"
#include "compiler/type_context.h"
#include "compiler/types.h"

struct FlowResult {
  enum class Status { Terminate, Fallthrough } status;
  std::optional<NarrowedBindings> narrowed_bindings = std::nullopt;

  bool is_terminated() const { return status == Status::Terminate; }
  bool is_fallthrough() const { return status == Status::Fallthrough; }
};

struct FunctionContext {
  std::vector<NamedBinding> required_captures;
  std::unordered_set<TypeId> required_template_variables;
  const SpannedType return_type;
};

struct LoopContext {
  NarrowedBindings initial_bindings;
  std::optional<NarrowedBindings> break_bindings;
  bool contains_break = false;
};

class SemanticAnalyzer {
 public:
  explicit SemanticAnalyzer(TypeContext& type_context,
                            ScopeManager& scope_manager,
                            ErrorCollector& error_collector,
                            TypeRegistry& type_registry);

  FlowResult Check(Block& block,
                   FunctionContext& context,
                   const NarrowedBindings& narrowed_bindings,
                   std::optional<LoopContext*> loop_context = std::nullopt);

 private:
  FlowResult Check(const std::unique_ptr<Statement>& statement,
                   FunctionContext& function_context,
                   const NarrowedBindings& existing_bindings,
                   std::optional<LoopContext*> loop_context = std::nullopt);

  NarrowedBindings UnionBindings(const NarrowedBindings& left,
                                 const NarrowedBindings& right,
                                 const NarrowedBindings& base);

  TypeContext& type_context_;
  ScopeManager& scope_manager_;
  ErrorCollector& error_collector_;
  TypeRegistry& type_registry_;
};