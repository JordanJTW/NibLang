// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <optional>
#include <unordered_map>
#include <vector>

#include "compiler/types.h"

// Maintains information related to lexical scoping within the AST.
class ScopeManager {
 public:
  enum ScopeType {
    RootScope,
    StructScope,
    FunctionScope,
    BlockScope,
    TemplateScope,
  };

  explicit ScopeManager();
  ~ScopeManager() = default;

  // Creates a new scope of `type` with `name` under the currently active scope.
  ScopeId EnterScope(ScopeType type, std::string_view name);
  // Exits the current scope, setting the active scope to its parent scope.
  void ExitScope();

  // Allows getting the current scope ID (allowing it to be restored).
  ScopeId GetActiveScopeId() const { return active_scope_id_; }

  // Looks up a binding by name within the given scope. By default, searches the
  // current scope and all parent scopes. If `scope` is Function, it will search
  // all parent scopes up to the nearest FunctionScope. If `scope` is Current,
  // it will only search the current scope.
  enum ScopeToCheck { Current, Function, Closure, All };
  std::optional<NamedBinding> FindBindingFor(
      std::string_view name,
      ScopeToCheck scope_to_check,
      std::optional<ScopeId> override_scope_id = std::nullopt) const;

  // Returns a NamedBinding of Kind::Variable declared in the current scope.
  NamedBinding DeclareVariableBinding(SpannedText name, TypeId type_id);
  // Returns a NamedBinding of Kind::Argument declared in the current scope.
  NamedBinding DeclareArgumentBinding(SpannedText name, TypeId type_id);
  // Returns a NamedBinding of Kind::Capture declared in the current scope.
  NamedBinding DeclareCaptureBinding(SpannedText name, TypeId type_id);
  // Returns a NamedBinding of Kind::Narrowed declared in the current scope.
  NamedBinding DeclareNarrowedBinding(NamedBinding binding_to_narrow,
                                      TypeId narrowed_type);
  // Returns a NamedBinding to Kind::Template declared in the current scope.
  NamedBinding DeclareTemplateBinding(SpannedText name, TypeId type_id);

  NamedBinding InsertNameIntoScope(
      SpannedText name,
      NamedBinding::Kind kind,
      std::optional<TypeId> type_id,
      std::optional<SymbolId> symbol_id,
      std::optional<NamedBinding::Idx> idx = std::nullopt,
      std::optional<TypeId> parent_type_id = std::nullopt);

  const auto& GetBindingsForScope(ScopeId scope_id) const {
    return scopes_[scope_id].bindings_for_scope;
  }

  template <typename Fn>
  auto WithScope(ScopeId scope_id, Fn&& block) {
    using Result = std::invoke_result_t<Fn>;

    ScopeId current_scope_id = GetActiveScopeId();
    SetActiveScopeId(scope_id);

    if constexpr (std::is_void_v<Result>) {
      block();
      SetActiveScopeId(current_scope_id);
    } else {
      Result result = block();
      SetActiveScopeId(current_scope_id);
      return result;
    }
  }

  template <typename Fn>
  auto NewScope(ScopeType type, std::string_view name, Fn&& block) {
    using Result = std::invoke_result_t<Fn>;

    EnterScope(type, name);
    Result result = block();
    ExitScope();
    return result;
  }

 private:
  friend std::ostream& operator<<(std::ostream&, const ScopeManager&);

  // Restores a previous `scope_id` as the currently active scope.
  void SetActiveScopeId(ScopeId scope_id);

  struct Scope {
    ScopeType scope_type;
    ScopeId parent_scope_id;
    std::string name;

    std::unordered_map<std::string, size_t> binding_lookup;
    std::vector<NamedBinding> bindings_for_scope;  // Preserve declaration order
    NamedBinding::Idx next_symbol_idx{0};  // Unique ID for variables in scope
    std::vector<ScopeId> children;
  };

  void PrintScopeTree(std::ostream&, ScopeId, size_t) const;

  std::vector<Scope> scopes_;
  ScopeId active_scope_id_{0};
  ScopeId function_scope_id_{0};
};

std::ostream& operator<<(std::ostream&, const ScopeManager&);
std::ostream& operator<<(std::ostream&, ScopeManager::ScopeType);
