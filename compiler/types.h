// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "compiler/tokenizer.h"

using SymbolId = size_t;
using ScopeId = size_t;
using TypeId = size_t;

struct FunctionDeclaration;
struct StructDeclaration;

struct InstanceHash {
  std::size_t operator()(const std::vector<TypeId>& argument_type_ids) const {
    std::size_t seed = argument_type_ids.size();
    for (const auto& id : argument_type_ids) {
      seed ^= std::hash<TypeId>{}(id) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    return seed;
  }
};

struct TypeInstance {
  TypeId type_id;
  ScopeId scope_id;
};

using InstanceCache =
    std::unordered_map<std::vector<TypeId>, TypeInstance, InstanceHash>;

struct FunctionSymbol {
  FunctionDeclaration& declaration;
  std::optional<StructDeclaration*> parent_declaration;
  SymbolId symbol_id;
  ScopeId scope_id;

  std::unordered_map<std::string, TypeId> default_template_type_ids;
  InstanceCache instances;

  bool IsExtern() const;
  std::string GetName() const;
};

struct StructSymbol {
  StructDeclaration& declaration;
  ScopeId scope_id;

  InstanceCache instances;
};

struct NamedBinding {
  using Idx = size_t;

  std::string name;
  enum Kind {
    Function,
    Struct,
    Field,
    Argument,
    Variable,
    Capture,
    Narrowed,
    Template,
    TypeAlias,
  } kind;
  std::optional<TypeId> realized_type_id;
  std::optional<SymbolId> symbol_id;

  inline bool IsRealized() { return realized_type_id.has_value(); }
  inline bool IsType() {
    return kind == Function || kind == Struct || kind == Template ||
           kind == TypeAlias;
  }

  // Used for function table resolution, struct field ordering, etc.
  std::optional<Idx> idx;

  std::optional<TypeId> parent_type_id;

  inline bool operator==(const NamedBinding& other) const {
    return kind == other.kind && realized_type_id == other.realized_type_id &&
           symbol_id == other.symbol_id && name == other.name &&
           idx == other.idx;
  }
};

std::ostream& operator<<(std::ostream& os, const NamedBinding& symbol);

struct Expression;
struct Statement;

struct ResolvedBinary {
  enum class Specialization { Number, String, Nil };
  Specialization specialization = Specialization::Number;
};

struct BinaryExpression {
  TokenKind op;
  std::unique_ptr<Expression> lhs;
  std::unique_ptr<Expression> rhs;
  std::optional<ResolvedBinary> resolved;
};

struct PrefixUnaryExpression {
  TokenKind op;
  std::unique_ptr<Expression> operand;
};

struct PostfixUnaryExpression {
  TokenKind op;
  std::unique_ptr<Expression> operand;
};

struct StringLiteral {
  std::string value;
};

struct CodepointLiteral {
  uint32_t value;
  std::string escaped_value;
};

using CallIdx = size_t;

struct ResolvedIdentifier {
  NamedBinding symbol;
};

struct Identifier {
  std::string name;
  std::optional<ResolvedIdentifier> resolved;
};

struct Nil {};

struct PrimaryExpression {
  std::variant<StringLiteral,
               CodepointLiteral,
               Identifier,
               int32_t,
               float,
               bool,
               Nil>
      value;
};

enum FunctionKind {
  Free,
  Extern,
  Anonymous,
  Method,
  StaticMethod,
  Constructor,
};

struct ResolvedCall {
  SymbolId target_symbol_id;
  FunctionKind kind;
};

struct CallExpression {
  std::unique_ptr<Expression> callee;
  std::vector<std::unique_ptr<Expression>> arguments;
  std::optional<ResolvedCall> resolved;
};

using MemberIdx = size_t;

struct ResolvedAccess {
  MemberIdx index;
};

struct MemberAccessExpression {
  std::unique_ptr<Expression> object;
  std::string member_name;
  std::optional<ResolvedAccess> resolved;
};

struct OptionalAccessExpression {
  std::unique_ptr<Expression> target;
};

struct ArrayAccessExpression {
  std::unique_ptr<Expression> array;
  std::unique_ptr<Expression> index;
};

struct AssignmentExpression {
  std::unique_ptr<Expression> lhs;
  std::unique_ptr<Expression> rhs;
};

// This is codegen'd very differently than BinaryExpression thus separating it.
struct LogicExpression {
  enum Kind { AND, OR } kind;
  std::unique_ptr<Expression> lhs;
  std::unique_ptr<Expression> rhs;
};

struct Block {
  std::vector<std::unique_ptr<Statement>> statements;
};

struct ParsedType;

struct ParsedUnionType {
  std::vector<ParsedType> names;
};

struct ParsedFunctionType {
  std::vector<ParsedType> arguments;
  std::shared_ptr<ParsedType> return_value;
};

struct ParsedOptionalType {
  std::shared_ptr<ParsedType> wrapped_type;
};

struct ParsedParameterizedType {
  std::shared_ptr<ParsedType> type;
  std::vector<ParsedType> parameters;
};

struct ParsedType {
  std::variant<std::string,
               ParsedUnionType,
               ParsedFunctionType,
               ParsedOptionalType,
               ParsedParameterizedType>
      type;
  Metadata metadata;
};

struct TypeAliasStatement {
  Token name;
  std::unique_ptr<ParsedType> type;
};

std::ostream& operator<<(std::ostream& os, const ParsedType& type);

enum class TypeCastStrategy {
  // "as" cast throws an exception if the cast is invalid.
  STRICT,
  // "as?" cast returns Nil (i.e. is an Optional) if the cast is invalid.
  OPTIONAL
};

struct TypeCastExpression {
  std::unique_ptr<Expression> expr;
  ParsedType as_type;
  TypeCastStrategy strategy;
};

struct NilCoalescingExpression {
  std::unique_ptr<Expression> lhs;
  std::unique_ptr<Expression> rhs;
};

struct ResolvedFunction {
  NamedBinding function_symbol;

  std::vector<NamedBinding> required_captures;
};

struct TemplateArgument {
  std::string name;
  std::optional<ParsedType> default_type;
};

struct FunctionDeclaration {
  // Function name is expected to be empty for `Anonymous` functions.
  std::string name;
  std::vector<std::pair<std::string, ParsedType>> arguments;
  ParsedType return_type;
  FunctionKind function_kind;
  std::vector<TemplateArgument> template_arguments;
  bool is_variadic = false;

  Metadata argument_range;

  std::unique_ptr<Block> body;
  std::optional<ResolvedFunction> resolved;
  size_t local_count = 0;
};

struct ClosureExpression {
  FunctionDeclaration fn;
};

struct OptionalChainExpression {
  std::unique_ptr<Expression> root;
};

struct TemplateInstantiationExpression {
  std::unique_ptr<Expression> generic_target;
  std::vector<ParsedType> template_types;
};

struct Expression {
  std::variant<PrimaryExpression,
               BinaryExpression,
               PrefixUnaryExpression,
               PostfixUnaryExpression,
               CallExpression,
               AssignmentExpression,
               MemberAccessExpression,
               ArrayAccessExpression,
               LogicExpression,
               ClosureExpression,
               TypeCastExpression,
               OptionalChainExpression,
               NilCoalescingExpression,
               OptionalAccessExpression,
               TemplateInstantiationExpression>
      as;

  Metadata meta;
  TypeId type;
};

struct ReturnStatement {
  std::unique_ptr<Expression> value;
};

struct ThrowStatement {
  std::unique_ptr<Expression> value;
};

struct IfStatement {
  std::unique_ptr<Expression> condition;
  Block then_body;
  Block else_body;
};

struct WhileStatement {
  std::unique_ptr<Expression> condition;
  Block body;
};

struct BreakStatement {};
struct ContinueStatement {};

struct StructDeclaration {
  std::string name;
  std::vector<TemplateArgument> template_arguments;
  std::vector<std::pair<std::string, ParsedType>> fields;
  std::vector<std::pair<std::string, FunctionDeclaration>> methods;
  bool is_extern;

  bool IsTemplate() const { return !template_arguments.empty(); }
};

struct AssignStatement {
  std::string name;
  std::optional<ParsedType> type;
  std::unique_ptr<Expression> value;
  std::optional<ResolvedIdentifier> resolved;
};

struct ImportStatement {
  std::string path;
};

struct Statement {
  std::variant<std::unique_ptr<Expression>,  // ExpressionStatement
               FunctionDeclaration,          // fn foo(x, y) {...}
               ReturnStatement,              // return <expr>;
               ThrowStatement,               // throw <expr>;
               IfStatement,                  // if (<expr>) {...} else { ... }
               WhileStatement,               // while (<expr>) {...}
               BreakStatement,               // break;
               ContinueStatement,            // continue;
               AssignStatement,              // let x: i32 = 42;
               StructDeclaration,            // struct Foo { ... }
               ImportStatement,              // @import "path/to/import"
               TypeAliasStatement            // type name = ParsedType;
               >
      as;
  Metadata meta;
};