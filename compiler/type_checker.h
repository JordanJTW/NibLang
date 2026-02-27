#pragma once

#include <map>
#include <set>
#include <vector>

#include "compiler/types.h"

class TypeChecker {
 public:
  explicit TypeChecker(const std::string& file);

  void Check(Block& block);

 private:
  void CheckStatement(std::unique_ptr<Statement>& statement);
  TypeId CheckExpression(std::unique_ptr<Expression>& expression);

  std::optional<TypeId> GetTypeIdFor(ParsedType type);

  std::optional<TypeId> DefineFunction(
      FunctionDeclaration& decl,
      std::optional<StructDeclaration*> object = std::nullopt);
  void CheckFunctionBody(const FunctionDeclaration& fn);

  enum class CreateIfMissing { YES, NO };
  std::optional<CallIdx> GetCallIdxFor(const std::string& name,
                                       CreateIfMissing create);

  struct Symbol {
    enum Kind { Function, Struct, Variable } kind;
    TypeId type_id;
  };
  std::optional<Symbol> GetSymbolFor(const std::string& ident,
                                     Metadata metadata);

  struct Scope {
    std::map<std::string, Symbol> symbols;
    std::optional<TypeId> return_type;
  };
  std::vector<Scope> scopes_;

  TypeId next_type_id_;

  CallIdx next_call_idx_ = 1;
  const std::string& file_;

  struct BuiltInType {};

  struct FunctionType {
    std::vector<TypeId> argument_types;
    TypeId return_type;
    FunctionKind kind;
    CallIdx call_idx;
    bool is_variadic;
  };

  struct MemberType {
    std::optional<MemberIdx> index_in_array;
    TypeId type_id;
  };

  struct StructType {
    std::map<std::string, MemberType> member_types;
    std::vector<TypeId> field_members;
    const StructDeclaration* parsed_struct;
    std::optional<CallIdx> constructor_call_idx;
  };

  struct UnionType {
    std::vector<TypeId> types;

    bool operator==(const UnionType other) const {
      return types == other.types;
    }
  };

  struct UnionTypeHash {
    size_t operator()(const UnionType& key) const {
      size_t h = 0;
      for (TypeId id : key.types) {
        h ^= std::hash<TypeId>{}(id) + 0x9e3779b9 + (h << 6) + (h >> 2);
      }
      return h;
    }
  };
  std::unordered_map<UnionType, TypeId, UnionTypeHash> interned_union_type_;

  using Type = std::variant<BuiltInType, FunctionType, StructType, UnionType>;
  std::unordered_map<TypeId, Type> type_info_;

  bool IsTypeSubsetOf(TypeId sub, TypeId super);
};