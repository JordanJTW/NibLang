#include "compiler/type_registry.h"

#include <sstream>
#include <string>

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

NamedBinding TypeRegistry::NewStructSymbol(StructDeclaration& declaration) {
  StructSymbol symbol = scope_manager_.NewScope(
      ScopeManager::StructScope, "struct " + declaration.name.text, [&]() {
        StructSymbol symbol = {declaration, scope_manager_.GetActiveScopeId()};

        for (auto& [name, fn] : declaration.methods) {
          NewFunctionSymbol(fn, &declaration);
        }
        return symbol;
      });

  std::optional<TypeId> type_id = std::nullopt;
  // If the `struct` is already realized at declaration (concrete) then assign
  // its TypeId so that conrete struct/function declarations will fully resolve.
  if (!declaration.IsTemplate()) {
    type_id = NewTypeId();
  }

  SymbolId symbol_id = next_symbol_id_++;
  symbol_table_.emplace(symbol_id, std::move(symbol));
  return scope_manager_.InsertNameIntoScope(
      declaration.name, NamedBinding::Struct, type_id, symbol_id);
}

SymbolId TypeRegistry::NewFunctionSymbol(
    FunctionDeclaration& declaration,
    std::optional<StructDeclaration*> parent_declaration) {
  SymbolId symbol_id = next_symbol_id_++;
  FunctionSymbol symbol{declaration, std::move(parent_declaration), symbol_id,
                        scope_manager_.GetActiveScopeId()};
  symbol_table_.emplace(symbol_id, std::move(symbol));

  scope_manager_.InsertNameIntoScope(declaration.name, NamedBinding::Function,
                                     /*type_id=*/std::nullopt, symbol_id);
  return symbol_id;
}

TypeId TypeRegistry::NewStructType(StructType type,
                                   std::optional<TypeId> self_id) {
  TypeId type_id = self_id.value_or(next_type_id_++);
  type_table_.emplace(type_id, std::move(type));
  return type_id;
}

TypeId TypeRegistry::NewFunctionType(FunctionType type) {
  if (const auto& it = interned_fn_type_.find(type);
      it != interned_fn_type_.end()) {
    return it->second;
  }

  TypeId type_id = next_type_id_++;
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

  TypeId type_id = next_type_id_++;
  interned_optional_type_[type] = type_id;
  type_table_[type_id] = OptionalType{type};
  return type_id;
}

TypeId TypeRegistry::NewUnionType(UnionType type) {
  if (const auto& it = interned_union_type_.find(type);
      it != interned_union_type_.end()) {
    return it->second;
  }

  TypeId type_id = next_type_id_++;
  interned_union_type_[type] = type_id;
  type_table_[type_id] = type;
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
                    {LiteralType::Void, "Void"},
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
          [&](const AliasType type) { return "Alias[" + type.name + "]"; }},
      it->second);
}

std::ostream& operator<<(std::ostream& os, const TypeRegistry& registry) {
  for (size_t id = 0; id < registry.next_type_id_; ++id) {
    os << id << ". " << registry.GetNameFromTypeId(id) << "\n";
  }
  return os;
}