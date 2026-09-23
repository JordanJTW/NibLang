// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/symbol_binder.h"

#include <ranges>

#include "compiler/error_collector.h"
#include "compiler/scope_manager.h"
#include "compiler/type_registry.h"
#include "compiler/type_rewriter.h"

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
      NewFunctionSymbol(*declaration, std::nullopt, std::nullopt);
    }
  }
}

SymbolBinder::StructBinding SymbolBinder::ForwardDeclareStruct(
    StructDeclaration& declaration) {
  TypeId type_id = type_registry_.NewTypeId();
  auto [symbol_id, symbol_ref] = type_registry_.NewStructSymbol(declaration);
  auto binding = scope_manager_.InsertNameIntoScope(
      declaration.name, NamedBinding::Struct, type_id, symbol_id);
  ;
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
                           alias.type->metadata);
      return;
    }

    if (!target_binding->IsTypeRef()) {
      error_collector_.Add("Cannot create type alias '" + alias.name.text +
                               "' from value identifier '" + *target + "'",
                           alias.type->metadata);
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
                                    std::optional<TypeId> self_id) {
  auto& declaration = struct_symbol.declaration;
  const std::string scope_name = "struct " + declaration.name.text;

  // Static Scope
  auto static_scope_guard =
      scope_manager_.NewScope(ScopeManager::StructSymbolScope, scope_name);
  struct_symbol.static_scope_id = scope_manager_.GetActiveScopeId();

  // Ensure that `template_variable_type_ids` is bound before processing
  // static methods as they may instantiate this template indirectly.
  struct_symbol.template_variable_type_ids =
      BindTemplateVariables(declaration.template_variables);

  // The generic struct type needs to be registered before static/methods or
  // fields because they might reference themselves and end up with a different
  // TypeId in that case.
  self_id = type_registry_.NewStructType(
      StructType{struct_symbol, struct_symbol.template_variable_type_ids},
      self_id);

  for (auto& fn : declaration.methods | std::views::values) {
    if (fn.function_kind == FunctionKind::StaticMethod) {
      // Intentionally not passing `self_id` for `static` methods.
      NewFunctionSymbol(fn, &declaration, std::nullopt);
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
                           type.metadata);
      type_id = TypeRegistry::Error;
    }

    scope_manager_.InsertNameIntoScope(name, NamedBinding::Field, *type_id,
                                       /*symbol_id=*/std::nullopt, field_idx++);
    struct_symbol.field_types.push_back(type_id.value());
  }

  for (auto& fn : declaration.methods | std::views::values) {
    if (fn.function_kind == FunctionKind::Method) {
      NewFunctionSymbol(fn, &declaration, *self_id);
    }
  }

  for (auto& [interface_name, implements_decl] : declaration.interfaces) {
    auto interface_binding =
        scope_manager_.FindBindingFor(interface_name.text, ScopeManager::All);

    if (!interface_binding || interface_binding->kind != NamedBinding::Struct) {
      error_collector_.Add(
          "unable to find interface '" + interface_name.text + "'",
          interface_name.metadata);
      continue;
    }

    const auto& interface_symbol =
        type_registry_.GetSymbolChecked<StructSymbol>(
            interface_binding->GetSymbolId());

    if (interface_symbol.template_variable_type_ids.size() !=
        implements_decl.template_types.size()) {
      error_collector_
          .Add("mismatched template argument count",
               implements_decl.name.metadata)
          .WithNote("declared here",
                    interface_symbol.declaration.name.metadata);
      continue;
    }

    SubstitutionMap substitution_map;
    substitution_map.reserve(
        interface_symbol.template_variable_type_ids.size());
    bool encountered_error = false;
    for (size_t i = 0; i < interface_symbol.template_variable_type_ids.size();
         ++i) {
      auto type_id =
          type_context_.GetTypeIdFor(implements_decl.template_types[i]);
      if (!type_id.has_value()) {
        std::stringstream ss;
        ss << "unknown type used as template argument: "
           << implements_decl.template_types[i];
        error_collector_.Add(ss.str(),
                             implements_decl.template_types[i].metadata);
        encountered_error = true;
        break;
      }

      substitution_map.insert(
          {interface_symbol.template_variable_type_ids[i], *type_id});
    }
    if (encountered_error)
      continue;

    std::unordered_map<std::string, NamedBinding> method_bindings;
    scope_manager_.NewScope(
        ScopeManager::BlockScope, "impl " + interface_name.text, [&]() {
          for (auto& [method_name, fn] : implements_decl.impls) {
            // TODO: `parent` should incorporate interface somehow for naming?
             if (auto binding = NewFunctionSymbol(fn, &declaration, *self_id)) {
              method_bindings.emplace(method_name.text, *binding);
            }
          }
          struct_symbol.interface_scopes.push_back(
              scope_manager_.GetActiveScopeId());
        });

    for (const auto& binding : scope_manager_.GetBindingsForScope(
             interface_symbol.instance_scope_id)) {
      CHECK_EQ(binding.kind, NamedBinding::Function)
          << "Only methods should be in an Interface scope";

      if (!method_bindings.contains(binding.name.text)) {
        error_collector_
            .Add("implementation of '" +
                     interface_symbol.declaration.name.text +
                     "' is missing method: '" + binding.name.text + "'",
                 implements_decl.name.metadata)
            .WithNote("defined here", binding.name.metadata);
        continue;
      }

      TypeId substituted_type = TypeRewriter(type_registry_, type_context_)
                                    .Rewrite(binding.type_id, substitution_map);
      if (substituted_type != method_bindings[binding.name.text].type_id) {
        error_collector_
            .Add("interface expects method '" + binding.name.text +
                     "' with type " +
                     type_registry_.GetNameFromTypeId(binding.type_id),
                 binding.name.metadata)
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
    std::optional<TypeId> self_id) {
  SymbolId symbol_id =
      type_registry_.NewFunctionSymbol(declaration, parent_declaration);

  auto& symbol = type_registry_.GetSymbolChecked<FunctionSymbol>(symbol_id);
  symbol.template_variable_type_ids =
      BindTemplateVariables(symbol.declaration.template_variables);

  if (symbol.IsMethodBodyRequired() && !declaration.body) {
    error_collector_.Add("non-extern functions MUST have a body",
                         declaration.name.metadata);
  }

  if (!symbol.IsExtern() && declaration.variadic_type) {
    error_collector_.Add("'...' is only allowed in extern functions",
                         declaration.variadic_type->variadic_span);
  }

  std::optional<TypeInstance> instance = type_context_.DeclareFunctionType(
      symbol, TypeContext::CheckFunctionBody::YES, self_id);
  if (!instance.has_value())
    return std::nullopt;

  symbol.canonical_type_id = instance->type_id;

  auto type_id = instance ? instance->type_id : TypeRegistry::Error;
  NamedBinding binding = scope_manager_.InsertNameIntoScope(
      declaration.name, NamedBinding::Function, type_id, symbol_id,
      /*idx=*/std::nullopt, self_id);

  declaration.resolved = ResolvedFunction{.function_symbol_id = symbol_id};
  return binding;
}
