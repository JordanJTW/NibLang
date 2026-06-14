// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/scope_manager.h"

#include "compiler/logging.h"

ScopeManager::ScopeManager() {
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

    if (auto found = current_scope.symbols.find(name.data());
        found != current_scope.symbols.end()) {
      return found->second;
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
    std::string_view name,
    NamedBinding::Kind kind,
    std::optional<TypeId> type_id,
    std::optional<SymbolId> symbol_id,
    std::optional<NamedBinding::Idx> idx,
    std::optional<TypeId> parent_type_id) {
  NamedBinding binding = {.kind = kind,
                          .name = name.data(),
                          .realized_type_id = std::move(type_id),
                          .symbol_id = std::move(symbol_id),
                          .idx = std::move(idx),
                          .parent_type_id = std::move(parent_type_id)};
  scopes_[active_scope_id_].symbols[name.data()] = binding;
  return binding;
}

NamedBinding ScopeManager::DeclareVariableBinding(std::string_view name,
                                                  TypeId type_id) {
  return InsertNameIntoScope(name, NamedBinding::Variable, type_id,
                             /*symbol_id=*/std::nullopt,
                             scopes_[function_scope_id_].next_symbol_idx++);
}

NamedBinding ScopeManager::DeclareCaptureBinding(std::string_view name,
                                                 TypeId type_id) {
  return InsertNameIntoScope(name, NamedBinding::Capture, type_id,
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

NamedBinding ScopeManager::DeclareTemplateBinding(std::string_view name,
                                                  TypeId type_id) {
  return InsertNameIntoScope(name, NamedBinding::Template, type_id,
                             /*symbol_id=*/std::nullopt);
}

std::vector<NamedBinding> ScopeManager::GetBindingsForScope(
    ScopeId scope_id,
    NamedBinding::Kind filter_kind) const {
  std::vector<NamedBinding> result;
  for (const auto& [name, binding] : scopes_[scope_id].symbols) {
    if (binding.kind == filter_kind)
      result.push_back(binding);
  }
  return result;
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

  if (!scope.symbols.empty()) {
    os << indent << "  Symbols:\n";
    for (const auto& [name, binding] : scope.symbols) {
      os << indent << "    - " << name << " = " << binding << std::endl;
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
