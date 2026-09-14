// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

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
  const TypeId return_type_id;
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