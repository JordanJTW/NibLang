#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "compiler/tokenizer.h"

using TypeId = size_t;

struct Symbol {
  using Idx = size_t;

  enum Kind { Function, Struct, Field, Variable, Capture } kind;
  TypeId type_id;
  // Used for function table resolution, struct field ordering, etc.
  std::optional<Idx> idx;

  inline bool operator==(const Symbol& other) const {
    return kind == other.kind && type_id == other.type_id && idx == other.idx;
  }
};

std::ostream& operator<<(std::ostream& os, const Symbol& symbol);

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

using CallIdx = size_t;

struct ResolvedIdentifier {
  Symbol symbol;
};

struct Identifier {
  std::string name;
  std::optional<ResolvedIdentifier> resolved;
};

struct Nil {};

struct PrimaryExpression {
  std::variant<StringLiteral, Identifier, int32_t, float, bool, Nil> value;
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
  CallIdx function_idx;
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

struct ParsedType {
  std::variant<std::string, ParsedUnionType, ParsedFunctionType> type;
  Metadata metadata;
};

std::ostream& operator<<(std::ostream& os, const ParsedType& type);

struct TypeCastExpression {
  std::unique_ptr<Expression> expr;
  ParsedType as_type;
};

struct ResolvedFunction {
  Symbol function_symbol;
  std::vector<Symbol> variables_to_capture;

  std::vector<Symbol> arguments;
  std::vector<Symbol> capture_arguments;
};

struct FunctionDeclaration {
  // Function name is expected to be empty for `Anonymous` functions.
  std::string name;
  std::vector<std::pair<std::string, ParsedType>> arguments;
  ParsedType return_type;
  FunctionKind function_kind;
  bool is_variadic = false;

  Metadata argument_range;

  std::unique_ptr<Block> body;
  std::optional<ResolvedFunction> resolved;
  size_t local_count = 0;
};

struct ClosureExpression {
  FunctionDeclaration fn;
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
               TypeCastExpression>
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
  std::vector<std::pair<std::string, ParsedType>> fields;
  std::vector<std::pair<std::string, FunctionDeclaration>> methods;
  bool is_extern;
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
               ImportStatement               // @import "path/to/import"
               >
      as;
  Metadata meta;
};