// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <optional>

#include "compiler/error_collector.h"
#include "compiler/scope_manager.h"
#include "compiler/type_context.h"
#include "compiler/type_registry.h"
#include "compiler/types.h"

class SymbolBinder {
 public:
  explicit SymbolBinder(ScopeManager& scope_manager,
                        TypeRegistry& type_registry,
                        TypeContext& type_context,
                        ErrorCollector& error_collector);

  void Process(const Block& block);

  using StructBinding = std::pair<NamedBinding, StructSymbol*>;
  StructBinding ForwardDeclareStruct(StructDeclaration& declaration);

 private:
  void ForwardDeclareTypeAlias(const TypeAliasStatement& alias);

  std::vector<TypeId> BindTemplateVariables(
      const std::vector<TemplateVariable>& template_variables);

  void BindStructSymbol(StructSymbol& struct_symbol,
                        std::optional<TypeId> self_id);
  std::optional<NamedBinding> NewFunctionSymbol(
      FunctionDeclaration& declaration,
      std::optional<const StructDeclaration*> parent_declaration,
      std::optional<TypeId> self_id);

  ScopeManager& scope_manager_;
  TypeRegistry& type_registry_;
  TypeContext& type_context_;
  ErrorCollector& error_collector_;
};