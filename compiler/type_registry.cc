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

inline void ComputeHash(size_t& seed, TypeId value) {
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

bool UnionType::operator==(const UnionType other) const {
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
  for (size_t i = 0; i < LiteralType::kCount; ++i)
    type_table_[i] = BuiltInType{};
}

std::pair<SymbolId, StructSymbol*> TypeRegistry::NewStructSymbol(
    StructSymbol symbol) {
  SymbolId symbol_id = next_symbol_id_++;
  auto [it, success] = symbol_table_.emplace(symbol_id, std::move(symbol));
  CHECK(success) << "SymbolId already exists in SymbolTable";
  return {symbol_id, std::get_if<StructSymbol>(&it->second)};
}

SymbolId TypeRegistry::NewFunctionSymbol(
    FunctionDeclaration& declaration,
    std::optional<const StructDeclaration*> parent_declaration) {
  SymbolId symbol_id = next_symbol_id_++;
  FunctionSymbol symbol{declaration, parent_declaration, symbol_id,
                        scope_manager_.GetActiveScopeId()};
  auto [it, success] = symbol_table_.emplace(symbol_id, std::move(symbol));
  CHECK(success) << "SymbolId already exists in SymbolTable";
  return symbol_id;
}

TypeId TypeRegistry::NewStructType(StructType type,
                                   std::optional<TypeId> self_id) {
  TypeId type_id = self_id.has_value() ? *self_id : NewTypeId();
  type_table_.emplace(type_id, std::move(type));
  return type_id;
}

TypeId TypeRegistry::NewFunctionType(FunctionType type) {
  if (const auto& it = interned_fn_type_.find(type);
      it != interned_fn_type_.end()) {
    return it->second;
  }

  TypeId type_id = NewTypeId();
  interned_fn_type_[type] = type_id;
  type_table_[type_id] = type;
  return type_id;
}

void TypeRegistry::NewAliasType(std::string_view name,
                                TypeId self_id,
                                TypeId target_id) {
  type_table_[self_id] = AliasType(name.data(), target_id);
}

TypeId TypeRegistry::NewOptionalType(TypeId type) {
  if (const auto& it = interned_optional_type_.find(type);
      it != interned_optional_type_.end()) {
    return it->second;
  }

  TypeId type_id = NewTypeId();
  interned_optional_type_[type] = type_id;
  type_table_[type_id] = OptionalType{type};
  return type_id;
}

TypeId TypeRegistry::NewUnionType(UnionType type) {
  if (const auto& it = interned_union_type_.find(type);
      it != interned_union_type_.end()) {
    return it->second;
  }

  TypeId type_id = NewTypeId();
  interned_union_type_[type] = type_id;
  type_table_[type_id] = type;
  return type_id;
}

TypeId TypeRegistry::NewPlaceholderType(SlotId idx) {
  if (const auto& it = interned_placeholder_type_.find(idx);
      it != interned_placeholder_type_.end()) {
    return it->second;
  }

  TypeId type_id = NewTypeId();
  type_table_[type_id] = PlaceholderType{idx};
  interned_placeholder_type_[idx] = type_id;
  return type_id;
}

TypeId TypeRegistry::NewTypeId() {
  return next_type_id_++;
}

std::string TypeRegistry::GetNameFromTypeId(TypeId type_id) const {
  auto it = type_table_.find(type_id);
  if (it == type_table_.end()) {
    return "Unknown";
  }

  return std::visit(
      Overloaded{
          [&](const BuiltInType& type) {
            static const std::unordered_map<TypeId, std::string>
                kBuiltInTypeNames = {
                    {LiteralType::Unit, "Unit"},
                    {LiteralType::i32, "i32"},
                    {LiteralType::f32, "f32"},
                    {LiteralType::Codepoint, "Codepoint"},
                    {LiteralType::Bool, "bool"},
                    {LiteralType::Any, "any"},
                    {LiteralType::Never, "never"},
                    {LiteralType::Nil, "Nil"},
                };
            return kBuiltInTypeNames.at(type_id);
          },
          [&](const FunctionType type) {
            std::stringstream ss;
            ss << "fn (";
            for (size_t i = 0; i < type.arg_types.size(); ++i) {
              if (i > 0)
                ss << ", ";
              ss << GetNameFromTypeId(type.arg_types[i]);
            }
            if (type.variadic_type) {
              if (!type.arg_types.empty())
                ss << ", ";
              ss << "..." << GetNameFromTypeId(*type.variadic_type);
            }
            ss << ") -> " << GetNameFromTypeId(type.return_type);
            return ss.str();
          },
          [&](const StructType type) {
            if (type.template_arguments.empty())
              return "struct " + type.declaration.name.text;

            std::stringstream ss;
            ss << "struct " << type.declaration.name.text << "[";
            for (size_t i = 0; i < type.template_arguments.size(); ++i) {
              if (i > 0)
                ss << ", ";
              ss << GetNameFromTypeId(type.template_arguments[i]);
            }
            ss << "]";
            return ss.str();
          },
          [&](const UnionType type) {
            std::stringstream ss;
            ss << "Union[";
            for (size_t i = 0; i < type.types.size(); ++i) {
              if (i > 0)
                ss << ", ";
              ss << GetNameFromTypeId(type.types[i]);
            }
            ss << "]";
            return ss.str();
          },
          [&](const OptionalType type) {
            return GetNameFromTypeId(type.wrapped_type) + "?";
          },
          [&](const AliasType type) { return "Alias[" + type.name + "]"; },
          [&](const PlaceholderType& type) {
            return "$" + std::to_string(type.idx);
          }},
      it->second);
}

std::string TypeRegistry::ToJson() const {
  nlohmann::json dict;
  for (const auto& [type_id, type] : type_table_) {
    dict["type_table"][type_id] = GetNameFromTypeId(type_id);
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