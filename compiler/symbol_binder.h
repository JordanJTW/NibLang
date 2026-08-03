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
  StructBinding BindStruct(StructDeclaration& declaration);

 private:
  void BindTypeAlias(const TypeAliasStatement& alias);

  ScopeManager& scope_manager_;
  TypeRegistry& type_registry_;
  TypeContext& type_context_;
  ErrorCollector& error_collector_;
};