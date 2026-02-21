#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "compiler/tokenizer.h"

struct Expression;

struct BinaryExpression {
  TokenKind op;
  std::unique_ptr<Expression> lhs;
  std::unique_ptr<Expression> rhs;
};

struct StringLiteral {
  std::string value;
};

struct Identifier {
  std::string name;
};

struct PrimaryExpression {
  std::variant<StringLiteral, Identifier, int32_t, float, bool> value;
};

using CallIdx = size_t;

enum FunctionKind { Free, Method };

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

struct ResolvedNew {
  std::string new_function;
};

struct NewExpression {
  std::string struct_name;
  std::vector<std::unique_ptr<Expression>> arguments;
  std::optional<ResolvedNew> resolved;
};

using TypeId = size_t;
using TextRange = std::pair<size_t, size_t>;

struct Expression {
  std::variant<PrimaryExpression,
               BinaryExpression,
               CallExpression,
               AssignmentExpression,
               MemberAccessExpression,
               ArrayAccessExpression,
               LogicExpression,
               NewExpression>
      as;

  TypeId type;
  TextRange text_range;
};

struct Statement;

struct Block {
  std::vector<std::unique_ptr<Statement>> statements;
};

struct FunctionDeclaration {
  std::string name;
  std::vector<std::pair<std::string, std::string>> arguments;
  std::vector<std::string> return_types;
  std::unique_ptr<Block> body;
  std::optional<CallIdx> call_idx;
};

struct ReturnStatement {
  std::unique_ptr<Expression> value;
};

struct ThrowStatement {
  std::unique_ptr<Expression> value;
};

struct IfStatement {
  size_t id;
  std::unique_ptr<Expression> condition;
  Block then_body;
  Block else_body;
};

struct WhileStatement {
  size_t id;
  std::unique_ptr<Expression> condition;
  Block body;
};

struct BreakStatement {};
struct ContinueStatement {};

struct StructDeclaration {
  std::string name;
  std::vector<std::pair<std::string, std::string>> fields;
  std::vector<std::pair<std::string, FunctionDeclaration>> methods;
  bool is_extern;
};

struct AssignStatement {
  std::string name;
  std::string type;
  std::unique_ptr<Expression> value;
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
               StructDeclaration             // struct Foo { ... }
               >
      as;
  TextRange text_range;
};

void print_statement(const Statement& stmt, size_t indent = 0);
void print_expression(const std::unique_ptr<Expression>& expr,
                      size_t indent = 0);