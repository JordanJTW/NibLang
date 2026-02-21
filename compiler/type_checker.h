#pragma once

#include <map>
#include <set>

#include "compiler/types.h"

class TypeChecker {
 public:
  explicit TypeChecker(const std::string& file);

  void Check(Block& block);

 private:
  void CheckStatement(std::unique_ptr<Statement>& statement);
  TypeId CheckExpression(std::unique_ptr<Expression>& expression);

  enum class CreateIfMissing { No, Yes };
  std::optional<TypeId> GetIdFor(std::set<std::string> sumtype,
                                 CreateIfMissing create);

  std::optional<TypeId> DefineFunction(FunctionDeclaration& decl,
                                       FunctionKind kind);
  void CheckFunctionBody(const FunctionDeclaration& fn);

  struct Symbol {
    enum Kind { Function, Struct, Variable } kind;
    TypeId type_id;
  };
  std::optional<Symbol> GetSymbolFor(const std::string& ident);

  struct Scope {
    std::map<std::string, Symbol> symbols;
    std::optional<TypeId> return_type;
  };
  std::vector<Scope> scopes_;

  std::map<std::string, TypeId> string_to_type_;
  TypeId next_type_id_;

  CallIdx next_call_idx_ = 1;
  const std::string& file_;

  struct FunctionType {
    std::vector<TypeId> argument_types;
    TypeId return_type;
    FunctionKind kind;
    CallIdx call_idx;
  };

  struct MemberType {
    std::optional<MemberIdx> index_in_array;
    TypeId type_id;
  };

  struct StructType {
    std::map<std::string, MemberType> member_types;
    std::vector<TypeId> field_members;
    const StructDeclaration* parsed_struct;
  };

  struct VariableType {
    TypeId type;
  };

  using Type = std::variant<FunctionType, StructType>;

  std::unordered_map<TypeId, Type> type_info_;
};