#include "compiler/symbol_binder.h"

#include <ranges>

#include "compiler/error_collector.h"
#include "compiler/scope_manager.h"
#include "compiler/type_registry.h"

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
      struct_bindings.push_back(BindStruct(*declaration));
  }

  // May refer to interface/structs (see above) and may be referenced by the
  // implementations of those interface/structs (see below).
  for (auto& statement : block.statements) {
    if (const auto* alias = std::get_if<TypeAliasStatement>(&statement->as))
      BindTypeAlias(*alias);
  }

  // Type-check the full struct bodies (and functions) once all types are known.
  // Errors are logged from within `DefineStructType` and `DefineFunction`.
  for (auto& [binding, struct_symbol] : struct_bindings) {
    scope_manager_.WithScope(struct_symbol->self_scope_id, [&]() {
      for (auto& fn : struct_symbol->declaration.methods | std::views::values) {
        if (fn.function_kind == FunctionKind::StaticMethod) {
          // Intentionally not passing `self_id` for `static` methods.
          type_context_.DefineFunction(
              type_registry_.NewFunctionSymbol(fn, &struct_symbol->declaration),
              TypeContext::CheckFunctionBody::YES);
        }
      }

      if (binding.realized_type_id) {  // Templated structs have no TypeId yet
        // DefineStructType() depends on method symbols already being populated.
        type_context_.DefineStructType(*binding.realized_type_id,
                                       *struct_symbol,
                                       /*template_arguments=*/{});
      }
    });
  }
  for (auto& statement : block.statements) {
    if (auto* declaration = std::get_if<FunctionDeclaration>(&statement->as)) {
      SymbolId symbol_id = type_registry_.NewFunctionSymbol(*declaration);
      type_context_.DefineFunction(symbol_id);
    }
  }
}

SymbolBinder::StructBinding SymbolBinder::BindStruct(
    StructDeclaration& declaration) {
  std::vector<SymbolId> method_symbols;
  // Create a single canonical Symbol for each method (shared across instances)
  for (auto& fn : declaration.methods | std::views::values) {
    if (fn.function_kind == FunctionKind::Method) {
      SymbolId method_id = type_registry_.NewFunctionSymbol(fn, &declaration);
      method_symbols.push_back(method_id);
    }
  }

  // Host for the `static` methods (and nests any instances for cleanliness)
  const ScopeId symbol_scope_id = scope_manager_.CreateScope(
      ScopeManager::StructSymbolScope, "struct " + declaration.name.text);
  StructSymbol symbol{declaration, symbol_scope_id, std::move(method_symbols)};

  std::optional<TypeId> type_id = std::nullopt;
  // If the `struct` is already realized at declaration (concrete) then assign
  // its TypeId so that concrete struct/function declarations will fully resolve
  if (!declaration.IsTemplate()) {
    type_id = type_registry_.NewTypeId();
  }

  auto [symbol_id, symbol_ref] =
      type_registry_.NewStructSymbol(std::move(symbol));
  auto binding = scope_manager_.InsertNameIntoScope(
      declaration.name, NamedBinding::Struct, type_id, symbol_id);
  return {std::move(binding), symbol_ref};
}

void SymbolBinder::BindTypeAlias(const TypeAliasStatement& alias) {
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
        alias.name, NamedBinding::TypeAlias, target_binding->realized_type_id,
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
  }
}