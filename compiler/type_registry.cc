// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/type_registry.h"

#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "compiler/logging.h"

namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

void ComputeHash(size_t& seed, TypeId value) {
  seed ^= std::hash<TypeId>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

}  // namespace

bool FunctionType::operator==(const FunctionType& other) const {
  return other.arg_types == arg_types && other.return_type == return_type;
}

size_t FunctionType::Hash::operator()(const FunctionType& key) const {
  size_t hash = 0;
  ComputeHash(hash, key.arg_types.size());
  ComputeHash(hash, key.return_type);
  // Ensure the presence of `variadic` is accounted for otherwise
  // (int, int) and (int, ...int) can hash to the same value.
  ComputeHash(hash, key.variadic_type.has_value());

  if (key.variadic_type)
    ComputeHash(hash, *key.variadic_type);

  for (TypeId arg : key.arg_types)
    ComputeHash(hash, arg);
  return hash;
}

bool IntersectionType::operator==(const IntersectionType& other) const {
  return other.types == types;
}

size_t IntersectionType::Hash::operator()(const IntersectionType& key) const {
  size_t hash = 0;
  for (TypeId arg : key.types)
    ComputeHash(hash, arg);
  return hash;
}

bool UnionType::operator==(const UnionType& other) const {
  return other.types == types;
}

size_t UnionType::Hash::operator()(const UnionType& key) const {
  size_t hash = 0;
  for (TypeId arg : key.types)
    ComputeHash(hash, arg);
  return hash;
}

TypeRegistry::TypeRegistry(ScopeManager& scope_manager)
    : scope_manager_(scope_manager) {
  // Add dummy entries for the built-in types to simplify logic.
  for (size_t i = 0; i < LiteralType::kCount; ++i) {
    type_table_.emplace(i, Type{BuiltInType{}});
  }
}

std::pair<SymbolId, StructSymbol*> TypeRegistry::NewStructSymbol(
    StructDeclaration& declaration) {
  SymbolId symbol_id = next_symbol_id_++;
  StructSymbol symbol{declaration, symbol_id};
  auto [it, success] = symbol_table_.emplace(symbol_id, std::move(symbol));
  CHECK(success) << "SymbolId already exists in SymbolTable";
  return {symbol_id, std::get_if<StructSymbol>(&it->second)};
}

SymbolId TypeRegistry::NewFunctionSymbol(
    FunctionDeclaration& declaration,
    std::optional<const StructDeclaration*> parent_declaration) {
  SymbolId symbol_id = next_symbol_id_++;
  FunctionSymbol symbol{declaration, parent_declaration, symbol_id};
  auto [it, success] = symbol_table_.emplace(symbol_id, std::move(symbol));
  CHECK(success) << "SymbolId already exists in SymbolTable";
  return symbol_id;
}

TypeId TypeRegistry::NewStructType(StructType type,
                                   std::optional<TypeId> self_id) {
  InstanceCache& cache = struct_instance_cache_[type.symbol.symbol_id];
  if (auto it = cache.find(type.instance_template_type_ids); it != cache.end())
    return it->second.type_id;

  std::unordered_set<TypeId> template_variables, placeholder_variables;
  for (const auto& argument_type_id : type.instance_template_type_ids) {
    AppendVariablesFrom(argument_type_id, template_variables,
                        placeholder_variables);
  }

  TypeId type_id = self_id.has_value() ? *self_id : NewTypeId();
  cache[type.instance_template_type_ids] = TypeInstance{type_id};

  type_table_.emplace(type_id,
                      Type{type, template_variables, placeholder_variables});
  return type_id;
}

TypeId TypeRegistry::NewFunctionType(FunctionType type) {
  if (const auto& it = interned_fn_type_.find(type);
      it != interned_fn_type_.end()) {
    return it->second;
  }

  std::unordered_set<TypeId> template_variables, placeholder_variables;
  for (const auto& argument_type_id : type.arg_types) {
    AppendVariablesFrom(argument_type_id, template_variables,
                        placeholder_variables);
  }
  AppendVariablesFrom(type.return_type, template_variables,
                      placeholder_variables);

  if (type.variadic_type) {
    AppendVariablesFrom(*type.variadic_type, template_variables,
                        placeholder_variables);
  }

  TypeId type_id = NewTypeId();
  interned_fn_type_[type] = type_id;
  type_table_.emplace(type_id, Type{std::move(type), template_variables,
                                    placeholder_variables});
  return type_id;
}

void TypeRegistry::NewAliasType(std::string_view name,
                                TypeId self_id,
                                TypeId target_id) {
  auto type = Type{AliasType(name.data(), target_id)};
  auto target_type = type_table_[target_id];
  type.template_variable_types = target_type.template_variable_types;
  type.placeholder_types = target_type.placeholder_types;
  type_table_.emplace(self_id, std::move(type));
}

TypeId TypeRegistry::NewAliasType(AliasType alias_type) {
  TypeId type_id = NewTypeId();
  auto target_type = type_table_[alias_type.target_type_id];
  auto type = Type{std::move(alias_type)};
  type.template_variable_types = target_type.template_variable_types;
  type.placeholder_types = target_type.placeholder_types;
  type_table_.emplace(type_id, std::move(type));
  return type_id;
}

TypeId TypeRegistry::NewOptionalType(TypeId wrapped_type) {
  if (const auto& it = interned_optional_type_.find(wrapped_type);
      it != interned_optional_type_.end()) {
    return it->second;
  }

  TypeId type_id = NewTypeId();
  interned_optional_type_[wrapped_type] = type_id;
  auto type = Type{OptionalType{wrapped_type}};
  auto target_type = type_table_[wrapped_type];
  type.template_variable_types = target_type.template_variable_types;
  type.placeholder_types = target_type.placeholder_types;
  type_table_.emplace(type_id, std::move(type));
  return type_id;
}

TypeId TypeRegistry::NewUnionType(UnionType type) {
  if (const auto& it = interned_union_type_.find(type);
      it != interned_union_type_.end()) {
    return it->second;
  }

  std::unordered_set<TypeId> template_variables, placeholder_variables;
  for (const auto& member_type_id : type.types) {
    AppendVariablesFrom(member_type_id, template_variables,
                        placeholder_variables);
  }

  TypeId type_id = NewTypeId();
  interned_union_type_[type] = type_id;
  type_table_.emplace(type_id, Type{std::move(type), template_variables,
                                    placeholder_variables});
  return type_id;
}

TypeId TypeRegistry::NewIntersectionType(IntersectionType type) {
  if (const auto& it = interned_intersection_type_.find(type);
      it != interned_intersection_type_.end()) {
    return it->second;
  }

  std::unordered_set<TypeId> template_variables, placeholder_variables;
  for (const auto& member_type_id : type.types) {
    AppendVariablesFrom(member_type_id, template_variables,
                        placeholder_variables);
  }

  TypeId type_id = NewTypeId();
  interned_intersection_type_[type] = type_id;
  type_table_.emplace(type_id, Type{std::move(type), template_variables,
                                    placeholder_variables});
  return type_id;
}

TypeId TypeRegistry::NewPlaceholderType(SlotId idx) {
  if (const auto& it = interned_placeholder_type_.find(idx);
      it != interned_placeholder_type_.end()) {
    return it->second;
  }

  TypeId type_id = NewTypeId();
  type_table_.emplace(type_id, Type{PlaceholderType{idx}, {}, {type_id}});
  interned_placeholder_type_[idx] = type_id;
  return type_id;
}

TypeId TypeRegistry::NewTemplateVariableType(
    SpannedText name,
    std::optional<SpannedType> constraint_type) {
  TypeId type_id = NewTypeId();
  auto type = TemplateVariableType(std::move(name), constraint_type);
  type_table_.emplace(type_id, Type{std::move(type), {type_id}, {}});
  return type_id;
}

TypeId TypeRegistry::NewTypeId() {
  return next_type_id_++;
}

std::string TypeRegistry::GetNameFromTypeId(TypeId type_id,
                                            FormatOptions options) const {
  auto it = type_table_.find(type_id);
  if (it == type_table_.end()) {
    return options.is_embedded_type ? "Unknown" : "'Unknown'";
  }

  return std::visit(
      Overloaded{
          [&](const BuiltInType&) {
            static const std::unordered_map<TypeId, std::string>
                kBuiltInTypeNames = {
                    {LiteralType::Error, "error"},
                    {LiteralType::Unit, "Unit"},
                    {LiteralType::i32, "i32"},
                    {LiteralType::f32, "f32"},
                    {LiteralType::Codepoint, "Codepoint"},
                    {LiteralType::Bool, "bool"},
                    {LiteralType::Any, "any"},
                    {LiteralType::Never, "never"},
                    {LiteralType::Nil, "Nil"},
                };
            if (options.is_embedded_type)
              return kBuiltInTypeNames.at(type_id);

            return "'" + kBuiltInTypeNames.at(type_id) + "'";
          },
          [&](const FunctionType& type) {
            std::stringstream ss;

            bool must_include_final_quote = false;
            if (!options.is_embedded_type) {
              must_include_final_quote = true;
              ss << "'";
            }

            ss << "fn (";
            options.is_embedded_type = true;
            for (size_t i = 0; i < type.arg_types.size(); ++i) {
              if (i > 0)
                ss << ", ";
              ss << GetNameFromTypeId(type.arg_types[i], options);
            }
            if (type.variadic_type) {
              if (!type.arg_types.empty())
                ss << ", ";
              ss << "..." << GetNameFromTypeId(*type.variadic_type, options);
            }
            ss << ") -> " << GetNameFromTypeId(type.return_type, options);
            if (must_include_final_quote)
              ss << "'";
            return ss.str();
          },
          [&](const StructType& type) {
            std::stringstream ss;

            bool must_include_final_quote = false;
            const auto& declaration = type.symbol.declaration;
            if (options.use_debug_names) {
              ss << declaration.kind << " " << declaration.name.text;
            } else if (options.is_embedded_type) {
              ss << declaration.name.text;
            } else {
              ss << "'" + declaration.name.text;
              must_include_final_quote = true;
            }

            if (!type.instance_template_type_ids.empty()) {
              ss << "[";
              for (size_t i = 0; i < type.instance_template_type_ids.size();
                   ++i) {
                if (i > 0)
                  ss << ", ";
                options.is_embedded_type = true;
                ss << GetNameFromTypeId(type.instance_template_type_ids[i],
                                        options);
              }
              ss << "]";
            }

            if (must_include_final_quote)
              ss << "'";
            return ss.str();
          },
          [&](const TemplateVariableType& type) {
            if (options.is_embedded_type) {
              return type.name.text;
            }
            if (options.use_debug_names) {
              return "$" + type.name.text;
            }
            return "template variable '" + type.name.text + "'";
          },
          [&](const UnionType& type) {
            std::stringstream ss;
            ss << "(";
            options.is_embedded_type = true;
            for (size_t i = 0; i < type.types.size(); ++i) {
              if (i > 0)
                ss << "|";
              ss << GetNameFromTypeId(type.types[i], options);
            }
            ss << ")";
            return ss.str();
          },
          [&](const IntersectionType& type) {
            std::stringstream ss;
            ss << "(";
            options.is_embedded_type = true;
            for (size_t i = 0; i < type.types.size(); ++i) {
              if (i > 0)
                ss << "&";
              ss << GetNameFromTypeId(type.types[i], options);
            }
            ss << ")";
            return ss.str();
          },
          [&](const OptionalType& type) {
            bool should_quote = !options.is_embedded_type;
            options.is_embedded_type = true;

            if (should_quote)
              return "'" + GetNameFromTypeId(type.wrapped_type, options) + "?'";

            return GetNameFromTypeId(type.wrapped_type, options) + "?";
          },
          [&](const AliasType& type) {
            if (options.is_embedded_type) {
              return type.name;
            }
            if (options.use_debug_names) {
              return "Alias[" + type.name + "]";
            }
            return "alias '" + type.name + "'";
          },
          [&](const PlaceholderType& type) {
            return "$" + std::to_string(type.idx);
          }},
      it->second.variant);
}

std::string TypeRegistry::ToJson() const {
  nlohmann::json dict;
  for (const auto& [type_id, type] : type_table_) {
    dict["type_table"][type_id]["name"] = GetNameFromTypeId(type_id);
  }

  for (const auto& [symbol_id, symbol] : symbol_table_) {
    if (const auto* fn = std::get_if<FunctionSymbol>(&symbol)) {
      auto to_json = [](const FunctionSymbol* symbol) -> nlohmann::json {
        nlohmann::json dict;
        dict["kind"] = "function";
        dict["name"] = symbol->GetName();
        return dict;
      };

      dict["symbol_table"][symbol_id] = to_json(fn);
    }
    if (const auto* st = std::get_if<StructSymbol>(&symbol)) {
      auto to_json = [](const StructSymbol* symbol) -> nlohmann::json {
        nlohmann::json dict;
        dict["kind"] = "struct";
        dict["name"] = symbol->declaration.name.text;
        return dict;
      };

      dict["symbol_table"][symbol_id] = to_json(st);
    }
  }
  return dict.dump(2);
}

std::ostream& operator<<(std::ostream& os, const TypeRegistry& registry) {
  for (size_t id = 0; id < registry.next_type_id_; ++id) {
    os << id << ". " << registry.GetNameFromTypeId(id) << "\n";
  }
  return os;
}

void TypeRegistry::AppendVariablesFrom(
    TypeId type_id,
    std::unordered_set<TypeId>& template_variables,
    std::unordered_set<TypeId>& placeholder_variables) {
  const auto& target_template_variables =
      type_table_[type_id].template_variable_types;
  template_variables.insert(target_template_variables.begin(),
                            target_template_variables.end());

  const auto& target_placeholder_variables =
      type_table_[type_id].placeholder_types;
  placeholder_variables.insert(target_placeholder_variables.begin(),
                               target_placeholder_variables.end());
}