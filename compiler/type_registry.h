#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "compiler/scope_manager.h"
#include "compiler/types.h"

struct AliasType {
  std::string name;
  TypeId target_type_id;
};

// Represents a LiteralType in the type table. Ensures there are no gaps.
struct BuiltInType {};

struct FunctionType {
  std::vector<TypeId> arg_types;
  TypeId return_type;
  bool is_variadic{false};

  bool operator==(const FunctionType& other) const;

  struct Hash {
    size_t operator()(const FunctionType& key) const;
  };
};

struct OptionalType {
  TypeId wrapped_type;
};

struct StructType {
  // TypeResolutionState resolution_state;
  const StructDeclaration& declaration;

  // A separate _ordered_ list of field types used for constructors.
  std::vector<TypeId> field_types;
  std::vector<TypeId> template_arguments;
  // Represents the lexical scope of the struct definition.
  ScopeId scope_id;
};

struct UnionType {
  std::vector<TypeId> types;

  bool operator==(const UnionType other) const;

  struct Hash {
    size_t operator()(const UnionType& key) const;
  };
};

using Type = std::variant<AliasType,
                          BuiltInType,
                          FunctionType,
                          OptionalType,
                          StructType,
                          UnionType>;

// A global registry of all Types/Symbols created during compilation.
//
// Methods are provided to intern the various Types into TypeIds with unique
// behavior for each Type i.e. FunctionTypes and UnionType are interned
// structurally by function signature while StructTypes are nominally typed.
class TypeRegistry {
 public:
  enum LiteralType : TypeId {
    Void = 0,
    i32,
    f32,
    Codepoint,
    Bool,
    Any,    // The "top" type (every type can be treated as ⊤)
    Never,  // The "bottom" type (⊥ can be treated as every type)
    Nil,
    kCount
  };

  explicit TypeRegistry(ScopeManager& scope_manager);

  // Creates a new StructSymbol for `declaration` in the symbol table and adds a
  // NamedBinding to it in the current scope. If the `declaration` is concrete
  // then a TypeId is assigned proactively (to be passed to DefineStructType).
  NamedBinding NewStructSymbol(StructDeclaration& declaration);

  // Creates a new FunctionSymbol for `declaration` in the symbol table and adds
  // a NamedBinding to it in the current scope. `parent_declaration` MUST be set
  // for method FunctionSymbols and should point to the owning parent struct.
  SymbolId NewFunctionSymbol(
      FunctionDeclaration& declaration,
      std::optional<StructDeclaration*> parent_declaration = std::nullopt);

  // Interns `type` into the registry as `self_id` (or a newly generated TypeId
  // if one is not provided). Returns the TypeId for `type`.
  TypeId NewStructType(StructType type, std::optional<TypeId> self_id);
  // Interns `type` into the registry by function signature. Returns the TypeId.
  TypeId NewFunctionType(FunctionType type);
  // Interns `type` into the registry by members. Returns the TypeId.
  TypeId NewUnionType(UnionType type);
  // Creates an Optional type wrapping `type_id` and returns Optional[T]'s ID.
  TypeId NewOptionalType(TypeId type_id);

  // Creates an alias with `name` linking `self_id` to `target_id`.
  void NewAliasType(std::string_view name, TypeId self_id, TypeId target_id);

  // Vends a new TypeId. The TypeId MUST be used (i.e. registered promptly).
  TypeId NewTypeId();

  // Returns a human readable representation of an interned `type_id`.
  std::string GetNameFromTypeId(TypeId type_id) const;

  template <typename T>
  const T* GetType(TypeId type_id) const {
    auto it = type_table_.find(type_id);
    return it != type_table_.end() ? std::get_if<T>(&it->second) : nullptr;
  }

  template <typename T>
  T* GetSymbol(SymbolId id) {
    auto it = symbol_table_.find(id);
    return it != symbol_table_.end() ? std::get_if<T>(&it->second) : nullptr;
  }

  template <typename T>
  const T* GetSymbol(SymbolId id) const {
    return const_cast<TypeRegistry*>(this)->GetSymbol<T>(id);
  }

  const auto& symbol_table() const { return symbol_table_; }
  const auto& type_table() const { return type_table_; }

 private:
  friend std::ostream& operator<<(std::ostream&, const TypeRegistry&);

  ScopeManager& scope_manager_;

  std::unordered_map<FunctionType, TypeId, FunctionType::Hash>
      interned_fn_type_;
  std::unordered_map<UnionType, TypeId, UnionType::Hash> interned_union_type_;
  // Maps a TypeId to the TypeId of Optional[TypeId]
  std::unordered_map<TypeId, TypeId> interned_optional_type_;

  std::unordered_map<TypeId, Type> type_table_;
  TypeId next_type_id_{LiteralType::kCount};  // TypeIds start after built-ins

  using Symbol = std::variant<FunctionSymbol, StructSymbol>;
  std::unordered_map<SymbolId, Symbol> symbol_table_;
  SymbolId next_symbol_id_{0};
};

std::ostream& operator<<(std::ostream&, const TypeRegistry&);
