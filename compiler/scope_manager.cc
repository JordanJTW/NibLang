// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/scope_manager.h"

#include "compiler/error_collector.h"
#include "compiler/logging.h"

ScopeManager::ScopeManager(ErrorCollector& error_collector)
    : error_collector_(error_collector) {
  EnterScope(ScopeType::RootScope, "<<root>>");
}

ScopeId ScopeManager::EnterScope(ScopeType type, std::string_view name) {
  ScopeId scope_id = scopes_.size();

  scopes_.push_back(Scope{type, active_scope_id_, name.data()});
  scopes_[active_scope_id_].children.push_back(scope_id);
  active_scope_id_ = scope_id;

  if (type == ScopeType::FunctionScope)
    function_scope_id_ = scope_id;

  return scope_id;
}

void ScopeManager::ExitScope() {
  SetActiveScopeId(scopes_[active_scope_id_].parent_scope_id);
}

void ScopeManager::SetActiveScopeId(ScopeId scope_id) {
  active_scope_id_ = scope_id;
  function_scope_id_ = 0;

  ScopeId id = active_scope_id_;
  while (id != 0 && scopes_[id].scope_type != ScopeType::FunctionScope) {
    id = scopes_[id].parent_scope_id;
  }

  if (scopes_[id].scope_type == ScopeType::FunctionScope)
    function_scope_id_ = id;
}

std::optional<NamedBinding> ScopeManager::FindBindingFor(
    std::string_view name,
    ScopeToCheck scope_to_check,
    std::optional<ScopeId> override_scope_id) const {
  size_t current_id = override_scope_id.value_or(active_scope_id_);

  bool encountered_first_function_scope = false;
  while (true) {
    const auto& current_scope = scopes_[current_id];

    if (auto found = current_scope.binding_lookup.find(name.data());
        found != current_scope.binding_lookup.end()) {
      return current_scope.bindings_for_scope[found->second];
    }

    if (current_scope.scope_type == ScopeType::FunctionScope) {
      if (scope_to_check == ScopeToCheck::Function)
        break;

      // Closures are allowed to escape their function scope to look for
      // captures in the surrounding outer function scope (up to one level).
      if (scope_to_check == ScopeToCheck::Closure &&
          encountered_first_function_scope) {
        break;
      }

      encountered_first_function_scope = true;
    }

    if (scope_to_check == ScopeToCheck::Current)
      break;

    // Indicates we have searched the "root" scope (prevent infinite recursion).
    if (current_id == 0 && current_scope.parent_scope_id == 0)
      break;

    current_id = current_scope.parent_scope_id;
  }

  return std::nullopt;
}

NamedBinding ScopeManager::InsertNameIntoScope(
    SpannedText name,
    NamedBinding::Kind kind,
    std::optional<TypeId> type_id,
    std::optional<SymbolId> symbol_id,
    std::optional<NamedBinding::Idx> idx,
    std::optional<TypeId> parent_type_id) {
  NamedBinding binding = {.name = name,
                          .kind = kind,
                          .realized_type_id = std::move(type_id),
                          .symbol_id = std::move(symbol_id),
                          .idx = std::move(idx),
                          .parent_type_id = std::move(parent_type_id)};
  if (auto existing_binding = FindBindingFor(name.text, ScopeToCheck::Current)) {
    error_collector_
        .Add("Binding for '" + name.text + "' conflicts with existing binding",
             name.metadata)
        .WithNote("original binding for: '" + existing_binding->name.text + "'",
                  existing_binding->name.metadata);
    return *existing_binding;
  }
  auto& scope = scopes_[active_scope_id_];
  size_t binding_idx = scope.bindings_for_scope.size();
  scope.binding_lookup[name.text] = binding_idx;
  scope.bindings_for_scope.push_back(binding);
  return binding;
}

NamedBinding ScopeManager::DeclareVariableBinding(SpannedText name,
                                                  TypeId type_id) {
  return InsertNameIntoScope(std::move(name), NamedBinding::Variable, type_id,
                             /*symbol_id=*/std::nullopt,
                             scopes_[function_scope_id_].next_symbol_idx++);
}

NamedBinding ScopeManager::DeclareArgumentBinding(SpannedText name,
                                                  TypeId type_id) {
  return InsertNameIntoScope(std::move(name), NamedBinding::Argument, type_id,
                             /*symbol_id=*/std::nullopt,
                             scopes_[function_scope_id_].next_symbol_idx++);
}

NamedBinding ScopeManager::DeclareCaptureBinding(SpannedText name,
                                                 TypeId type_id) {
  return InsertNameIntoScope(std::move(name), NamedBinding::Capture, type_id,
                             /*symbol_id=*/std::nullopt,
                             scopes_[function_scope_id_].next_symbol_idx++);
}

NamedBinding ScopeManager::DeclareNarrowedBinding(
    NamedBinding binding_to_narrow,
    TypeId narrowed_type) {
  // Narrowed symbols shadow existing symbols, so assign the same index.
  return InsertNameIntoScope(binding_to_narrow.name, NamedBinding::Narrowed,
                             narrowed_type, binding_to_narrow.symbol_id,
                             binding_to_narrow.idx);
}

NamedBinding ScopeManager::DeclareTemplateBinding(SpannedText name,
                                                  TypeId type_id) {
  return InsertNameIntoScope(std::move(name), NamedBinding::Template, type_id,
                             /*symbol_id=*/std::nullopt);
}

void ScopeManager::PrintScopeTree(std::ostream& os,
                                  ScopeId current_id,
                                  size_t indent_level) const {
  if (current_id >= scopes_.size())
    return;

  const auto& scope = scopes_[current_id];
  std::string indent(indent_level * 2, ' ');

  os << indent << "[Scope " << current_id << "] '" << scope.name
     << "' (Type: " << scope.scope_type << ", Parent: " << scope.parent_scope_id
     << ")" << std::endl;

  if (!scope.bindings_for_scope.empty()) {
    os << indent << "  Symbols:\n";
    for (const auto& [name, idx] : scope.binding_lookup) {
      os << indent << "    - " << name << " = " << scope.bindings_for_scope[idx]
         << std::endl;
    }
  }

  for (size_t child_id : scope.children) {
    // Avoid infinite recursion if a child links back to itself or root
    if (child_id == current_id || child_id == 0)
      continue;

    PrintScopeTree(os, child_id, indent_level + 1);
  }
}

std::ostream& operator<<(std::ostream& os, const ScopeManager& self) {
  self.PrintScopeTree(os, /*current_id=*/0 /*root*/, /*indent_level=*/0);
  return os;
}

std::ostream& operator<<(std::ostream& os, ScopeManager::ScopeType type) {
#define CASE_NAME($name)               \
  case ScopeManager::ScopeType::$name: \
    return os << #$name

  switch (type) {
    CASE_NAME(BlockScope);
    CASE_NAME(RootScope);
    CASE_NAME(StructScope);
    CASE_NAME(FunctionScope);
    CASE_NAME(TemplateScope);
  }
  NOTREACHED() << "all ScopeTypes MUST be handled in switch()";
  return os;
}
