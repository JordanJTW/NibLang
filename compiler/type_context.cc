// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/type_context.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "compiler/error_collector.h"
#include "compiler/logging.h"
#include "compiler/parser/tokenizer.h"
#include "src/vm.h"
#include "type_rewriter.h"

namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

using LiteralType = TypeRegistry::LiteralType;

}  // namespace

TypeContext::TypeContext(ScopeManager& scope_manager,
                         TypeRegistry& type_registry,
                         ErrorCollector& error_collector)
    : scope_manager_(scope_manager),
      type_registry_(type_registry),
      error_collector_(error_collector) {}

std::optional<TypeId> TypeContext::GetTypeIdFor(const ParsedType& type) {
  return std::visit(
      Overloaded{
          [&](const std::string& type_name) -> std::optional<TypeId> {
            static const std::unordered_map<std::string, TypeId> kBuiltInTypes =
                {
                    {"Unit", LiteralType::Unit},
                    {"i32", LiteralType::i32},
                    {"Codepoint", LiteralType::Codepoint},
                    {"f32", LiteralType::f32},
                    {"bool", LiteralType::Bool},
                    {"any", LiteralType::Any},
                    {"never", LiteralType::Never},
                    // Nil is used with Optional types (T?), NEVER standalone
                    // {"Nil", LiteralType::Nil},
                };

            // Fast path for built-in type names which are used frequently.
            if (const auto& it = kBuiltInTypes.find(type_name);
                it != kBuiltInTypes.end()) {
              return it->second;
            }

            // Structure types are resolved nominally, while Functions would be
            // resolved structurally to allow for flexible callbacks, etc.
            if (auto binding = scope_manager_.FindBindingFor(
                    type_name, ScopeManager::All)) {
              if (!binding->IsTypeRef()) {
                // This can only happen in edge-cases like a variable appearing
                // in the scope from an import or when instantiating a template.
                error_collector_.Add("variable can not be used as a type",
                                     type.metadata);
                return std::nullopt;
              }

              // We can assume that types with template arguments have a Symbol
              if (!binding->symbol_id)
                return binding->type_id;

              if (auto* struct_symbol = type_registry_.GetSymbol<StructSymbol>(
                      binding->GetSymbolId())) {
                // Catches `Array` without an argument list. If an argument
                // list was provided then the ParseType would've been
                // `ParsedParameterizedType('Array', [T])`.
                if (!struct_symbol->template_variable_type_ids.empty()) {
                  error_collector_.Add("invalid use of template-name '" +
                                           type_name +
                                           "' without an argument list",
                                       type.metadata);
                  return std::nullopt;
                }
                return binding->type_id;
              }

              NOTREACHED() << "If reached; likely need to add checks to "
                              "missing template arguments :^)";
              return binding->type_id;
            }

            error_collector_.Add("unknown type '" + type_name + "'",
                                 type.metadata);
            return std::nullopt;
          },
          [&](const ParsedUnionType& type) -> std::optional<TypeId> {
            std::vector<TypeId> type_ids;
            for (const auto& name : type.names) {
              std::optional<TypeId> type_id = GetTypeIdFor(name);
              if (!type_id.has_value())
                return std::nullopt;

              type_ids.push_back(type_id.value());
            }
            CHECK_GT(type_ids.size(), 1)
                << "Parser returned a weird ParsedUnionType";

            return GetUnionOf(type_ids);
          },
          [&](const ParsedIntersectionType& type) -> std::optional<TypeId> {
            std::vector<TypeId> type_ids;
            for (const auto& name : type.names) {
              std::optional<TypeId> type_id = GetTypeIdFor(name);
              if (!type_id.has_value())
                return std::nullopt;

              type_ids.push_back(type_id.value());
            }
            CHECK_GT(type_ids.size(), 1)
                << "Parser returned a weird ParsedIntersectionType";

            return GetIntersectionOf(type_ids);
          },
          [&](const ParsedFunctionType& type) -> std::optional<TypeId> {
            std::vector<TypeId> arg_types;
            for (const auto& arg : type.arguments) {
              if (auto id = GetTypeIdFor(arg); id.has_value()) {
                arg_types.push_back(*id);
              } else {
                return std::nullopt;
              }
            }

            std::optional<TypeId> return_type =
                type.return_value ? GetTypeIdFor(*type.return_value)
                                  : LiteralType::Unit;
            if (!return_type.has_value())
              return std::nullopt;

            FunctionType key = {std::move(arg_types), return_type.value()};
            return type_registry_.NewFunctionType(std::move(key));
          },
          [&](const ParsedOptionalType& optional_type)
              -> std::optional<TypeId> {
            CHECK(optional_type.wrapped_type)
                << "Parser should never return an OptionalType without a "
                   "wrapped type";

            auto wrapped_type_id = GetTypeIdFor(*optional_type.wrapped_type);
            if (!wrapped_type_id.has_value())
              return std::nullopt;

            return type_registry_.NewOptionalType(*wrapped_type_id);
          },
          [&](const ParsedParameterizedType& parameterized_type)
              -> std::optional<TypeId> {
            const std::string& base_type_identifier =
                std::get<std::string>(parameterized_type.type->type);

            auto binding = scope_manager_.FindBindingFor(base_type_identifier,
                                                         ScopeManager::All);

            if (!binding || !binding->symbol_id) {
              error_collector_.Add(
                  "unknown base type `" + base_type_identifier + "`",
                  parameterized_type.type->metadata);
              return std::nullopt;
            }

            return GetTemplateOf(binding.value(),
                                 parameterized_type.parameters);
          }},
      type.type);
}

TypeId TypeContext::GetOptionalOf(TypeId type_id) {
  return type_registry_.NewOptionalType(type_id);
}

std::optional<TypeId> TypeContext::UnwrapOptional(TypeId type_id) const {
  if (const auto* optional = type_registry_.GetType<OptionalType>(type_id))
    return optional->wrapped_type;

  return std::nullopt;
}

TypeId TypeContext::GetUnionOf(const std::vector<TypeId>& types) {
  // Look-up member types and normalize (sort/dedupe/flatten).
  std::set<TypeId> normalized_types;  // std::set sorts and dedupes
  for (const auto& type_id : types) {
    // If the member itself is a union, flatten it.
    if (auto* u = type_registry_.GetType<UnionType>(type_id)) {
      normalized_types.insert(u->types.begin(), u->types.end());
    } else if (auto* o = type_registry_.GetType<OptionalType>(type_id)) {
      normalized_types.insert(o->wrapped_type);
      normalized_types.insert(LiteralType::Nil);
    } else {
      normalized_types.insert(type_id);
    }
  }

  // Never represents an impossible value, so T | Never simplifies to T.
  normalized_types.erase(LiteralType::Never);

  bool wrap_in_optional = false;
  if (normalized_types.contains(LiteralType::Nil)) {
    normalized_types.erase(LiteralType::Nil);
    wrap_in_optional = true;
  }

  // Handles `Hashable | Foo` => `Hashable`
  FlattenSubtypesUnion(normalized_types);

  // If all members collapsed away, the result is Never (or Nil).
  if (normalized_types.empty())
    return wrap_in_optional ? LiteralType::Nil : LiteralType::Never;

  // If after normalization the union is just a single type, then return it.
  if (normalized_types.size() == 1) {
    const TypeId type_id = *normalized_types.begin();
    return wrap_in_optional ? GetOptionalOf(type_id) : type_id;
  }

  // Intern unions structurally to a TypeId for faster comparisons.
  // UnionType stores the types as a std::vector instead of a std::set
  // to take advantage of better cache-locality (due to contiguous memory).
  auto key = UnionType{{normalized_types.begin(), normalized_types.end()}};
  const TypeId type_id = type_registry_.NewUnionType(std::move(key));
  return wrap_in_optional ? GetOptionalOf(type_id) : type_id;
}

TypeId TypeContext::GetIntersectionOf(const std::vector<TypeId>& types) {
  // Look-up member types and normalize (sort/dedupe/flatten).
  std::set<TypeId> intersection_types;  // std::set sorts and dedupes
  std::vector<std::vector<TypeId>> union_types;
  for (const auto& type_id : types) {
    // If the member itself is an intersection, flatten it.
    if (auto* u = type_registry_.GetType<IntersectionType>(type_id)) {
      intersection_types.insert(u->types.begin(), u->types.end());
    } else if (auto* u = type_registry_.GetType<UnionType>(type_id)) {
      union_types.push_back(u->types);
    } else if (auto* o = type_registry_.GetType<OptionalType>(type_id)) {
      // Optional type `T?` is a union: `T | Nil` (see GetUnionOf).
      union_types.push_back({o->wrapped_type, LiteralType::Nil});
    } else {
      intersection_types.insert(type_id);
    }
  }

  FlattenSubtypesIntersection(intersection_types);

  // An intersection with `Never` immediately collapses to `Never`
  if (intersection_types.contains(LiteralType::Never))
    return LiteralType::Never;

  // If no there are no union types to distribute/normalize less work!
  if (union_types.empty()) {
    if (intersection_types.empty())
      return LiteralType::Never;
    if (intersection_types.size() == 1)
      return *intersection_types.begin();

    auto key = IntersectionType{
        {intersection_types.begin(), intersection_types.end()}};
    return type_registry_.NewIntersectionType(std::move(key));
  }

  // Cartesian Product Expansion (Distributive Law):
  // https://proofwiki.org/wiki/Cartesian_Product_Distributes_over_Union
  // This ensures types are normalized to "Disjunctive normal form"
  // https://en.wikipedia.org/wiki/Disjunctive_normal_form
  std::vector<TypeId> resulting_intersection_types;
  std::function<void(size_t, std::vector<TypeId>&)> generate_combinations =
      [&](size_t union_index, std::vector<TypeId>& current_combination) {
        if (union_index == union_types.size()) {
          std::vector<TypeId> combination(intersection_types.begin(),
                                          intersection_types.end());
          combination.insert(combination.end(), current_combination.begin(),
                             current_combination.end());

          // Allows disjoint intersection to collapse to `Never`, etc.
          resulting_intersection_types.push_back(
              GetIntersectionOf(combination));
          return;
        }

        for (auto member_id : union_types[union_index]) {
          current_combination.push_back(member_id);
          generate_combinations(union_index + 1, current_combination);
          current_combination.pop_back();
        }
      };

  std::vector<TypeId> initial_combination;
  generate_combinations(0, initial_combination);

  // Allows any of the products that collapsed to `Never` to be stripped
  return GetUnionOf(resulting_intersection_types);
}

bool TypeContext::IsTypeNilable(TypeId type_id) const {
  return type_id == LiteralType::Nil || UnwrapOptional(type_id);
}

std::optional<TypeInstance> TypeContext::DeclareFunctionType(
    FunctionSymbol& symbol,
    CheckFunctionBody check_fn_body,
    std::optional<TypeId> self_id) {
  auto& fn = symbol.declaration;

  std::unique_ptr<ScopeGuard> template_scope_guard;
  if (!symbol.template_variable_type_ids.empty()) {
    template_scope_guard = scope_manager_.NewScope(ScopeManager::TemplateScope,
                                                   "fn " + fn.name.text);

    for (const auto& type_id : symbol.template_variable_type_ids) {
      const auto& template_variable =
          type_registry_.GetTypeChecked<TemplateVariableType>(type_id);
      scope_manager_.DeclareTemplateBinding(template_variable.name, type_id);
    }
  }

  auto instance_scope_guard = scope_manager_.NewScope(
      ScopeManager::FunctionInstanceScope, "fn " + fn.name.text);
  symbol.instance_scope_id = scope_manager_.GetActiveScopeId();

  if (self_id) {
    // Inject an implicit `self` for accessing methods/fields within the method.
    scope_manager_.DeclareArgumentBinding({"self", fn.name.metadata}, *self_id);
  }

  std::vector<TypeId> argument_types;
  for (const auto& [name, type] : fn.arguments) {
    if (auto type_id = GetTypeIdFor(type); type_id.has_value()) {
      scope_manager_.DeclareArgumentBinding(name, *type_id);
      argument_types.push_back(*type_id);
    } else {
      return std::nullopt;
    }
  }

  std::optional<TypeId> return_type = GetTypeIdFor(fn.return_type);
  // Missing return types are already handled in the Parser (resolving to
  // Void) so if `return_type` has no value here it is truly an unknown type.
  if (!return_type.has_value())
    return std::nullopt;

  std::optional<TypeId> variadic_type;
  if (fn.variadic_type.has_value()) {
    variadic_type = GetTypeIdFor(fn.variadic_type->type);

    if (!variadic_type.has_value())
      return std::nullopt;
  }

  ScopeId scope_id = scope_manager_.GetActiveScopeId();

  if (check_fn_body == CheckFunctionBody::YES)
    realized_functions_.push_back(RealizedFunction{scope_id, fn, *return_type});

  auto key = FunctionType{std::move(argument_types), return_type.value(),
                          std::move(variadic_type)};
  TypeId type_id = type_registry_.NewFunctionType(std::move(key));
  return TypeInstance{type_id, scope_id};
}

bool TypeContext::IsTypeSubsetOf(TypeId sub_type_id,
                                 TypeId super_type_id) const {
  // Fast-path without unwrapping aliases just compare the IDs.
  if (sub_type_id == super_type_id)
    return true;

  auto follow_alias = [this](TypeId type_id) {
    while (const auto* alias = type_registry_.GetType<AliasType>(type_id))
      type_id = alias->target_type_id;

    return type_id;
  };

  sub_type_id = follow_alias(sub_type_id);
  super_type_id = follow_alias(super_type_id);

  // Re-compare the type IDs after unwrapping aliases.
  if (sub_type_id == super_type_id)
    return true;

  if (sub_type_id == LiteralType::Never || super_type_id == LiteralType::Any)
    return true;

  if (sub_type_id == LiteralType::Error || super_type_id == LiteralType::Error)
    return true;

  const Type& sub_type = type_registry_.type_table().at(sub_type_id);
  const Type& super_type = type_registry_.type_table().at(super_type_id);

  if (!std::holds_alternative<UnionType>(sub_type) &&
      std::holds_alternative<UnionType>(super_type)) {
    const auto& super_types = std::get<UnionType>(super_type).types;
    return std::binary_search(super_types.begin(), super_types.end(),
                              sub_type_id);
  }

  if (std::holds_alternative<UnionType>(sub_type) &&
      !std::holds_alternative<UnionType>(super_type)) {
    return false;  // a union cannot be subset of a single concrete type
  }

  if (std::holds_alternative<UnionType>(sub_type) &&
      std::holds_alternative<UnionType>(super_type)) {
    const auto& sub_types = std::get<UnionType>(sub_type).types;
    const auto& super_types = std::get<UnionType>(super_type).types;

    return std::all_of(sub_types.begin(), sub_types.end(), [&](TypeId id) {
      return std::binary_search(super_types.begin(), super_types.end(), id);
    });
  }

  if (sub_type_id == LiteralType::Nil &&
      std::holds_alternative<OptionalType>(super_type)) {
    return true;  // Nil is a subset of any Optional type.
  }

  if (std::holds_alternative<OptionalType>(sub_type) &&
      std::holds_alternative<OptionalType>(super_type)) {
    const auto& sub_wrapped = std::get<OptionalType>(sub_type).wrapped_type;
    const auto& super_wrapped = std::get<OptionalType>(super_type).wrapped_type;
    return IsTypeSubsetOf(sub_wrapped, super_wrapped);
  }

  if (!std::holds_alternative<OptionalType>(sub_type) &&
      std::holds_alternative<OptionalType>(super_type)) {
    const auto& super_wrapped = std::get<OptionalType>(super_type).wrapped_type;
    return IsTypeSubsetOf(sub_type_id, super_wrapped);
  }

  if (std::holds_alternative<StructType>(sub_type) &&
      std::holds_alternative<StructType>(super_type)) {
    const auto& sub = std::get<StructType>(sub_type);
    const auto& super = std::get<StructType>(super_type);

    if (!sub.symbol.declaration.IsInterface() &&
        super.symbol.declaration.IsInterface()) {
      return sub.symbol.interface_types.contains(super_type_id);
    }
    // Intentional fall-through
  }

  // Handles `T` being passed from one generic function to another.
  if (std::holds_alternative<TemplateVariableType>(sub_type) &&
      std::holds_alternative<StructType>(super_type)) {
    const auto& super = std::get<StructType>(super_type);
    const auto& sub = std::get<TemplateVariableType>(sub_type);

    if (super.symbol.declaration.IsInterface() && sub.constraint_type_id) {
      return sub.constraint_type_id->type_id == super_type_id;
    }
    // Intentional fall-through
  }

  // Placeholders should _only_ be used during type inference in which case we
  // do not want to trigger false errors when checking constraints, etc.
  // TODO: Should PlaceholderType(s) contain the same constraints as their var?
  if (std::holds_alternative<PlaceholderType>(sub_type))
    return true;

  // Neither type is a union so they must be different concrete types.
  return false;
}

bool TypeContext::AreDisjointTypes(TypeId t1, TypeId t2) const {
  if (t1 == t2)  // T & T => T
    return false;

  auto* t1_type = type_registry_.GetType<StructType>(t1);
  auto* t2_type = type_registry_.GetType<StructType>(t2);

  bool t1_is_interface = t1_type && t1_type->symbol.declaration.IsInterface();
  bool t2_is_interface = t2_type && t2_type->symbol.declaration.IsInterface();

  if (t1_is_interface && t2_is_interface)  // i.e. Hashable & Printable
    return false;

  if (t1_is_interface && t2_type) {  // i.e. Hashable & Foo
    for (auto implemented_id : t2_type->symbol.interface_types) {
      if (implemented_id == t1)
        return false;
    }
  }

  if (t2_is_interface && t1_type) {  // i.e. Foo & Hashable
    for (auto implemented_id : t1_type->symbol.interface_types) {
      if (implemented_id == t2)
        return false;
    }
  }

  // Default fallback for primitives vs structs, etc.
  return true;
}

std::optional<TypeId> TypeContext::GetGenericTemplateOf(
    NamedBinding binding,
    const std::vector<TypeId>& template_type_ids) {
  std::vector<Metadata> template_spans;
  template_spans.reserve(template_type_ids.size());

  for (const auto& type_id : template_type_ids) {
    const auto& template_type =
        type_registry_.GetTypeChecked<TemplateVariableType>(type_id);
    template_spans.push_back(template_type.name.metadata);
  }

  return GetTemplateOf(std::move(binding), template_type_ids, template_spans);
}

std::optional<TypeId> TypeContext::GetTemplateOf(
    NamedBinding binding,
    const std::vector<ParsedType>& argument_types) {
  std::vector<TypeId> argument_type_ids;
  std::vector<Metadata> argument_spans;
  argument_type_ids.reserve(argument_types.size());
  argument_spans.reserve(argument_types.size());

  bool encountered_type_error = false;
  for (const auto& type : argument_types) {
    if (auto type_id = GetTypeIdFor(type)) {
      argument_type_ids.push_back(type_id.value());
      argument_spans.push_back(type.metadata);
    } else {
      // Keep parsing the rest of the types even if an error is
      // encountered with one to give as many errors as possible.
      std::stringstream ss;
      ss << "unknown type used as template argument: " << type;
      error_collector_.Add(ss.str(), type.metadata);
      encountered_type_error = true;
    }
  }
  if (encountered_type_error)
    return std::nullopt;

  return GetTemplateOf(std::move(binding), argument_type_ids, argument_spans);
}

std::optional<TypeId> TypeContext::GetTemplateOf(
    NamedBinding binding,
    const std::vector<TypeId>& argument_type_ids,
    const std::vector<Metadata>& argument_spans) {
  CHECK(binding.symbol_id) << "Provided binding is missing SymbolId";
  CHECK_EQ(argument_type_ids.size(), argument_spans.size());

  std::stringstream ss;
  for (size_t i = 0; i < argument_type_ids.size(); ++i) {
    if (i > 0)
      ss << ", ";

    TypeRegistry::FormatOptions options{.is_embedded_type = true};
    ss << type_registry_.GetNameFromTypeId(argument_type_ids[i], options);
  }

  auto check_template_constraints =
      [&](const std::vector<TypeId>& template_variable_type_ids) -> bool {
    bool violated_constraint = false;
    for (size_t i = 0; i < argument_type_ids.size(); ++i) {
      // TODO: Should this be a CHECK error? Can this happen normally?
      if (i >= template_variable_type_ids.size())
        return false;

      const auto& template_variable_type =
          type_registry_.GetTypeChecked<TemplateVariableType>(
              template_variable_type_ids[i]);

      if (!template_variable_type.constraint_type_id)  // Skip unconstrained 'T'
        continue;

      const auto& [constraint_type_id, constraint_span] =
          *template_variable_type.constraint_type_id;
      if (!IsTypeSubsetOf(argument_type_ids[i], constraint_type_id)) {
        error_collector_
            .Add(type_registry_.GetNameFromTypeId(argument_type_ids[i]) +
                     " does not satisfy interface constraint " +
                     type_registry_.GetNameFromTypeId(constraint_type_id),
                 argument_spans[i])
            .WithNote("constraint declared here", constraint_span);
        violated_constraint = true;
      }
    }
    return violated_constraint;
  };

  if (StructSymbol* symbol =
          type_registry_.GetSymbol<StructSymbol>(*binding.symbol_id)) {
    if (auto it = symbol->instances.find(argument_type_ids);
        it != symbol->instances.end()) {
      return it->second.type_id;
    }

    const auto& template_arguments = symbol->declaration.template_variables;

    if (argument_type_ids.size() < template_arguments.size()) {
      error_collector_.Add(
          "Template struct " + symbol->declaration.name.text + " requires " +
              std::to_string(template_arguments.size()) +
              " template arguments but only " +
              std::to_string(argument_type_ids.size()) + " were provided",
          {});
      return std::nullopt;
    }

    if (check_template_constraints(symbol->template_variable_type_ids))
      return std::nullopt;

    return type_registry_.NewStructType({*symbol, argument_type_ids},
                                        std::nullopt);
  }

  if (FunctionSymbol* symbol =
          type_registry_.GetSymbol<FunctionSymbol>(*binding.symbol_id)) {
    if (auto it = symbol->instances.find(argument_type_ids);
        it != symbol->instances.end()) {
      return it->second.type_id;
    }

    const auto& template_arguments = symbol->declaration.template_variables;

    if (argument_type_ids.size() < template_arguments.size()) {
      error_collector_.Add("Template fn " + symbol->GetName() + " requires " +
                               std::to_string(template_arguments.size()) +
                               " template arguments but only " +
                               std::to_string(argument_type_ids.size()) +
                               " were provided",
                           {});
      return std::nullopt;
    }

    if (check_template_constraints(symbol->template_variable_type_ids))
      return std::nullopt;

    std::unordered_map<TypeId, TypeId> type_ids;
    for (size_t i = 0; i < argument_type_ids.size(); ++i) {
      type_ids[symbol->template_variable_type_ids[i]] = argument_type_ids[i];
    }

    return TypeRewriter(type_registry_, *this)
        .Rewrite(symbol->canonical_type_id, type_ids);
  }

  NOTREACHED() << "Do not know how to realize binding: " << binding;
  return std::nullopt;
}

void TypeContext::FlattenSubtypesUnion(std::set<TypeId>& types) const {
  std::set<TypeId> non_intersection_types;
  for (TypeId type_id : types) {
    if (!type_registry_.GetType<IntersectionType>(type_id)) {
      non_intersection_types.insert(type_id);
    }
  }

  std::set<TypeId> type_ids_to_remove;
  for (const auto& type_id : types) {
    // Handle absorption: `A | (A & B) => A`
    if (auto* i = type_registry_.GetType<IntersectionType>(type_id)) {
      for (TypeId component : i->types) {
        if (non_intersection_types.contains(component)) {
          type_ids_to_remove.insert(type_id);
          break;
        }
      }
      continue;
    }

    // Handles `Foo | Hashable => Hashable` since `Foo` is one member of the
    // "set" that is `Hashable` (assuming `Foo` implements `Hashable`).
    // Union widens constraints to the broadest common supertype.
    if (const auto* struct_type = type_registry_.GetType<StructType>(type_id)) {
      for (const auto& potential_interface : types) {
        if (type_id == potential_interface)
          continue;

        if (struct_type->symbol.interface_types.contains(potential_interface)) {
          type_ids_to_remove.insert(type_id);
          break;
        }
      }
    }
  }

  if (type_ids_to_remove.empty())
    return;

  std::erase_if(types, [&](TypeId type_id) {
    return type_ids_to_remove.contains(type_id);
  });
}

void TypeContext::FlattenSubtypesIntersection(std::set<TypeId>& types) const {
  std::set<TypeId> type_ids_to_remove;
  for (const auto& type_id : types) {
    for (const auto& potential_interface : types) {
      if (type_id == potential_interface)
        continue;

      // `Foo & Hashable` => `Foo`
      // NOTE: This implicitly handles the absorption rule `A & (A | B) => A`
      // since IsTypeSubsetOf() treats a union as a supertype of its members.
      if (IsTypeSubsetOf(type_id, potential_interface)) {
        type_ids_to_remove.insert(potential_interface);
        continue;
      }

      // If two types are disjoint the entire intersection collapses to `Never`.
      if (AreDisjointTypes(type_id, potential_interface)) {
        types = {LiteralType::Never};
        return;
      }
    }
  }

  if (type_ids_to_remove.empty())
    return;

  std::erase_if(types, [&](TypeId type_id) {
    return type_ids_to_remove.contains(type_id);
  });
}
