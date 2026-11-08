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

#include "compiler/symbol_binder.h"

#include <ranges>

#include "compiler/error_collector.h"
#include "compiler/scope_manager.h"
#include "compiler/type_registry.h"
#include "compiler/type_rewriter.h"

namespace {

std::string AppendQualifiedName(const std::string_view prefix,
                                const std::string_view name) {
  if (prefix.empty())
    return std::string(name);

  return std::string(prefix) + "_" + std::string(name);
}

}  // namespace

SymbolBinder::SymbolBinder(ScopeManager& scope_manager,
                           TypeRegistry& type_registry,
                           TypeContext& type_context,
                           ErrorCollector& error_collector)
    : scope_manager_(scope_manager),
      type_registry_(type_registry),
      type_context_(type_context),
      error_collector_(error_collector) {}

void SymbolBinder::Process(const Block& block) {
  // Create bindings for nominal types (interfaces/structs) to ensure they can
  // be referenced regardless of declaration order in implementations.
  std::vector<StructBinding> struct_bindings;
  for (const auto& statement : block.statements) {
    if (auto* declaration = std::get_if<StructDeclaration>(&statement->as))
      struct_bindings.push_back(ForwardDeclareStruct(*declaration));
  }

  // May refer to interface/structs (see above) and may be referenced by the
  // implementations of those interface/structs (see below).
  for (auto& statement : block.statements) {
    if (const auto* alias = std::get_if<TypeAliasStatement>(&statement->as))
      ForwardDeclareTypeAlias(*alias);
  }

  // Type-check the full struct bodies (and functions) once all types are known.
  // Errors are logged from within `DefineStructType` and `DefineFunction`.
  for (auto& [binding, struct_symbol] : struct_bindings) {
    BindStructSymbol(*struct_symbol, binding.type_id);
  }

  for (auto& statement : block.statements) {
    if (auto* declaration = std::get_if<FunctionDeclaration>(&statement->as)) {
      NewFunctionSymbol(*declaration, /*parent_declaration=*/std::nullopt,
                        /*self_id=*/std::nullopt,
                        /*namespace_prefix=*/"");
    }
  }
}

SymbolBinder::StructBinding SymbolBinder::ForwardDeclareStruct(
    StructDeclaration& declaration) {
  // Allow late-binding String while keeping consistent ID for "string literal"
  TypeId type_id =
      (declaration.name.text == "String" ? TypeRegistry::String
                                         : type_registry_.NewTypeId());
  auto [symbol_id, symbol_ref] = type_registry_.NewStructSymbol(declaration);
  auto binding = scope_manager_.InsertNameIntoScope(
      declaration.name, NamedBinding::Struct, type_id, symbol_id);
  return {std::move(binding), symbol_ref};
}

std::vector<TypeId> SymbolBinder::BindTemplateVariables(
    const std::vector<TemplateVariable>& template_variables) {
  std::vector<TypeId> template_variable_type_ids;
  template_variable_type_ids.reserve(template_variables.size());

  for (const auto& [name, default_type, constraint_type] : template_variables) {
    std::optional<SpannedType> constraint_span_type;
    if (constraint_type) {
      if (auto type_id = type_context_.GetTypeIdFor(
              *constraint_type)) {  // Errors logged in `GetTypeIdFor`
        constraint_span_type = SpannedType{*type_id, constraint_type->metadata};
      } else {
        constraint_span_type =
            SpannedType{TypeRegistry::Error, constraint_type->metadata};
      }
    }
    template_variable_type_ids.push_back(
        type_registry_.NewTemplateVariableType(name, constraint_span_type));
  }
  return template_variable_type_ids;
}

void SymbolBinder::ForwardDeclareTypeAlias(const TypeAliasStatement& alias) {
  // Handles aliasing a specific type-ref (struct/interface).
  // No new TypeId is allocated, acts as direct links to underlying type.
  if (auto* target = std::get_if<std::string>(&alias.type->type)) {
    std::optional<NamedBinding> target_binding =
        scope_manager_.FindBindingFor(*target, ScopeManager::All);
    if (!target_binding) {
      error_collector_.Add("unknown type `" + *target + "`",
                           alias.type->metadata, FROM_HERE);
      return;
    }

    if (!target_binding->IsTypeRef()) {
      error_collector_.Add("Cannot create type alias '" + alias.name.text +
                               "' from value identifier '" + *target + "'",
                           alias.type->metadata, FROM_HERE);
      return;
    }

    scope_manager_.InsertNameIntoScope(
        alias.name, NamedBinding::TypeAlias, target_binding->type_id,
        target_binding->symbol_id, target_binding->idx,
        target_binding->parent_type_id);
    return;
  }

  // In the case of an alias to a more complex type we do assign a new
  // type to allow for recursive definitions i.e. alias Foo = Array[Foo].
  // All alias are nominally typed by definition so assign a new TypeId.
  TypeId type_id = type_registry_.NewTypeId();
  scope_manager_.InsertNameIntoScope(alias.name, NamedBinding::TypeAlias,
                                     type_id,
                                     /*symbol_id=*/std::nullopt);

  if (auto target_type_id = type_context_.GetTypeIdFor(*alias.type)) {
    type_registry_.NewAliasType(alias.name.text, type_id, *target_type_id);
  } else {
    type_registry_.NewAliasType(alias.name.text, type_id, TypeRegistry::Error);
  }
}

void SymbolBinder::BindStructSymbol(StructSymbol& struct_symbol,
                                    TypeId self_id) {
  auto& declaration = struct_symbol.declaration;
  const std::string struct_name = declaration.name.text;
  const std::string scope_name = "struct " + struct_name;

  // Static Scope
  auto static_scope_guard =
      scope_manager_.NewScope(ScopeManager::StructSymbolScope, scope_name);
  struct_symbol.static_scope_id = scope_manager_.GetActiveScopeId();

  if (struct_symbol.declaration.IsInterface() &&
      !struct_symbol.declaration.template_variables.empty()) {
    error_collector_.Add(
        "template variables are not allowed on `interface` declarations",
        struct_symbol.declaration.name.metadata, FROM_HERE);
    // Intentional fall-through to prevent cascading missing symbol errors
  }

  // Ensure that `template_variable_type_ids` is bound before processing
  // static methods as they may instantiate this template indirectly.
  struct_symbol.template_variable_type_ids =
      BindTemplateVariables(declaration.template_variables);

  // The generic struct type needs to be registered before static/methods or
  // fields because they might reference themselves and end up with a different
  // TypeId in that case.
  self_id = type_registry_.NewStructType(
      StructType{&struct_symbol, struct_symbol.template_variable_type_ids},
      self_id);

  for (auto& fn : declaration.methods | std::views::values) {
    if (fn.function_kind == FunctionKind::StaticMethod) {
      // Intentionally not passing `self_id` for `static` methods.
      NewFunctionSymbol(fn, &declaration, std::nullopt,
                        /*namespace_prefix=*/struct_name);
    }
  }

  // Template Scope
  auto template_scope_guard =
      scope_manager_.NewScope(ScopeManager::TemplateScope, scope_name);
  for (const auto& type_id : struct_symbol.template_variable_type_ids) {
    const auto& template_variable =
        type_registry_.GetTypeChecked<TemplateVariableType>(type_id);
    scope_manager_.DeclareTemplateBinding(template_variable.name, type_id);
  }

  // Instance Scope
  auto instance_scope_guard =
      scope_manager_.NewScope(ScopeManager::StructInstanceScope, scope_name);
  struct_symbol.instance_scope_id = scope_manager_.GetActiveScopeId();

  NamedBinding::Idx field_idx = 0;
  for (const auto& [name, type] : declaration.fields) {
    auto type_id = type_context_.GetTypeIdFor(type);
    if (!type_id.has_value()) {
      error_collector_.Add("Unknown type for struct field: " + name.text,
                           type.metadata, FROM_HERE);
      type_id = TypeRegistry::Error;
    }

    scope_manager_.InsertNameIntoScope(name, NamedBinding::Field, *type_id,
                                       /*symbol_id=*/std::nullopt, field_idx++);
    struct_symbol.field_types.push_back(type_id.value());
  }

  for (auto& fn : declaration.methods | std::views::values) {
    if (fn.function_kind == FunctionKind::Method) {
      if (declaration.IsInterface() && !fn.template_variables.empty()) {
        error_collector_.Add(
            "template variables are not allowed on `interface` methods",
            fn.name.metadata, FROM_HERE);
        // Intentional fall-through to prevent cascading missing symbol errors
      }

      NewFunctionSymbol(fn, &declaration, self_id,
                        /*namespace_prefix=*/struct_name);
    }
  }

  for (auto& [interface_name, implements_decl] : declaration.interfaces) {
    auto interface_binding =
        scope_manager_.FindBindingFor(interface_name.text, ScopeManager::All);

    if (!interface_binding || interface_binding->kind != NamedBinding::Struct) {
      error_collector_.Add(
          "unable to find interface '" + interface_name.text + "'",
          interface_name.metadata, FROM_HERE);
      continue;
    }

    if (!implements_decl.template_types.empty()) {
      error_collector_.Add(
          "template variables are not allowed on `interface` declarations",
          implements_decl.name.metadata, FROM_HERE);
      continue;
    }

    std::unordered_map<std::string, NamedBinding> method_bindings;
    scope_manager_.NewScope(
        ScopeManager::BlockScope, "impl " + interface_name.text, [&]() {
          for (auto& [method_name, fn] : implements_decl.impls) {
            // TODO: `parent` should incorporate interface somehow for naming?
            if (auto binding = NewFunctionSymbol(
                    fn, &declaration, self_id,
                    AppendQualifiedName(struct_name, interface_name.text))) {
              method_bindings.emplace(method_name.text, *binding);
            }
          }
          struct_symbol.interface_scopes.push_back(
              scope_manager_.GetActiveScopeId());
        });

    const auto& interface_symbol =
        type_registry_.GetSymbolChecked<StructSymbol>(
            interface_binding->GetSymbolId());

    SubstitutionMap substitution_map;
    substitution_map[TypeRegistry::Self] = self_id;
    for (const auto& binding : scope_manager_.GetBindingsForScope(
             interface_symbol.instance_scope_id)) {
      CHECK_EQ(binding.kind, NamedBinding::Function)
          << "Only methods should be in an Interface scope";

      if (!method_bindings.contains(binding.name.text)) {
        error_collector_
            .Add("implementation of '" +
                     interface_symbol.declaration.name.text +
                     "' is missing method: '" + binding.name.text + "'",
                 implements_decl.name.metadata, FROM_HERE)
            .WithNote("defined here", binding.name.metadata);
        continue;
      }

      TypeId substituted_type = TypeRewriter(type_registry_, type_context_)
                                    .Rewrite(binding.type_id, substitution_map);
      if (substituted_type != method_bindings[binding.name.text].type_id) {
        error_collector_
            .Add("interface expects method '" + binding.name.text +
                     "' with type " +
                     type_registry_.GetNameFromTypeId(substituted_type),
                 binding.name.metadata, FROM_HERE)
            .WithNote("instead has type " +
                          type_registry_.GetNameFromTypeId(
                              method_bindings[binding.name.text].type_id),
                      method_bindings[binding.name.text].name.metadata);
        continue;
      }

      type_registry_.GetSymbolChecked<FunctionSymbol>(*binding.symbol_id)
          .implementations.emplace(
              struct_symbol.symbol_id,
              *method_bindings[binding.name.text].symbol_id);
    }

    TypeId substituted_type =
        TypeRewriter(type_registry_, type_context_)
            .Rewrite(interface_binding->type_id, substitution_map);
    struct_symbol.interface_types.push_back(substituted_type);
  }
}

std::optional<NamedBinding> SymbolBinder::NewFunctionSymbol(
    FunctionDeclaration& declaration,
    std::optional<const StructDeclaration*> parent_declaration,
    std::optional<TypeId> self_id,
    std::string_view namespace_prefix) {
  SymbolId symbol_id = type_registry_.NewFunctionSymbol(
      declaration, parent_declaration,
      AppendQualifiedName(namespace_prefix, declaration.name.text));

  auto& symbol = type_registry_.GetSymbolChecked<FunctionSymbol>(symbol_id);
  symbol.template_variable_type_ids =
      BindTemplateVariables(symbol.declaration.template_variables);

  if (symbol.IsFunctionBodyRequired() && !declaration.body) {
    error_collector_.Add("non-extern functions MUST have a body",
                         declaration.name.metadata, FROM_HERE);
  }

  if (!symbol.IsExtern() && declaration.variadic_type) {
    error_collector_.Add("'...' is only allowed in extern functions",
                         declaration.variadic_type->variadic_span, FROM_HERE);
  }

  TypeId type_id = type_context_.DeclareFunctionType(symbol, self_id);
  symbol.canonical_type_id = type_id;

  NamedBinding binding = scope_manager_.InsertNameIntoScope(
      declaration.name, NamedBinding::Function, type_id, symbol_id,
      /*idx=*/std::nullopt, self_id);
  declaration.resolved = ResolvedFunction{
      .function_symbol_id = symbol_id,
      .required_template_variables =
          type_registry_.type_table().at(type_id).template_variable_types};
  return binding;
}
