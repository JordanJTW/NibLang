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

struct CallExpression {
  std::unique_ptr<Expression> callee;
  std::vector<std::unique_ptr<Expression>> arguments;
};

struct MemberAccessExpression {
  std::unique_ptr<Expression> object;
  std::string member_name;
};

struct ArrayAccessExpression {
  std::unique_ptr<Expression> array;
  std::unique_ptr<Expression> index;
};

struct AssignmentExpression {
  std::unique_ptr<Expression> lhs;
  std::unique_ptr<Expression> rhs;
};

struct Expression {
  std::variant<PrimaryExpression,
               BinaryExpression,
               CallExpression,
               AssignmentExpression,
               MemberAccessExpression,
               ArrayAccessExpression>
      as;
};

struct Statement;

struct Block {
  std::vector<std::unique_ptr<Statement>> statements;
};

struct FunctionDeclaration {
  std::string name;
  std::vector<std::pair<std::string, std::string>> arguments;
  std::vector<std::string> return_types;
  Block body;
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

struct Statement {
  std::variant<std::unique_ptr<Expression>,  // ExpressionStatement
               FunctionDeclaration,          // fn foo(x, y) {...}
               ReturnStatement,              // return <expr>;
               ThrowStatement,               // throw <expr>;
               IfStatement,                  // if (<expr>) {...} else { ... }
               WhileStatement,               // while (<expr>) {...}
               BreakStatement,               // break;
               ContinueStatement             // continue;
               >
      as;
};

void print_statement(const Statement& stmt, size_t indent = 0);