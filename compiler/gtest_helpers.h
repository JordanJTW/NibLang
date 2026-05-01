// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "compiler/types.h"

namespace testing {

class ExprBuilder {
 public:
  std::unique_ptr<Expression> node;

  explicit ExprBuilder(std::unique_ptr<Expression> n) : node(std::move(n)) {}

  ExprBuilder(const ExprBuilder&) = delete;
  ExprBuilder& operator=(const ExprBuilder&) = delete;

  ExprBuilder(ExprBuilder&&) noexcept = default;
  ExprBuilder& operator=(ExprBuilder&&) noexcept = default;

  operator std::unique_ptr<Expression>() { return std::move(node); }

  ExprBuilder member(std::string name) && {
    return ExprBuilder(std::make_unique<Expression>(
        Expression{MemberAccessExpression{std::move(node), std::move(name)}}));
  }

  ExprBuilder at(std::unique_ptr<Expression> index) && {
    return ExprBuilder(std::make_unique<Expression>(
        Expression{ArrayAccessExpression{std::move(node), std::move(index)}}));
  }

  template <typename... Args>
  ExprBuilder call(Args&&... args) && {
    std::vector<std::unique_ptr<Expression>> args_exprs;
    args_exprs.reserve(sizeof...(Args));
    (args_exprs.push_back(std::move(args.node)), ...);
    return ExprBuilder(std::make_unique<Expression>(
        Expression{CallExpression{std::move(node), std::move(args_exprs)}}));
  }
};

ExprBuilder string(std::string value) {
  return ExprBuilder(std::make_unique<Expression>(
      Expression{PrimaryExpression{StringLiteral{std::move(value)}}}));
}

ExprBuilder ident(std::string name) {
  return ExprBuilder(std::make_unique<Expression>(
      Expression{PrimaryExpression{Identifier{std::move(name)}}}));
}

ExprBuilder i32(int32_t value) {
  return ExprBuilder(
      std::make_unique<Expression>(Expression{PrimaryExpression{value}}));
}

ExprBuilder f32(float value) {
  return ExprBuilder(
      std::make_unique<Expression>(Expression{PrimaryExpression{value}}));
}

ExprBuilder boolean(bool value) {
  return ExprBuilder(
      std::make_unique<Expression>(Expression{PrimaryExpression{value}}));
}

ExprBuilder binary(TokenKind op,
                   std::unique_ptr<Expression> lhs,
                   std::unique_ptr<Expression> rhs) {
  return ExprBuilder(std::make_unique<Expression>(
      Expression{BinaryExpression{op, std::move(lhs), std::move(rhs)}}));
}

ExprBuilder member(std::unique_ptr<Expression> object, std::string_view name) {
  return ExprBuilder(std::make_unique<Expression>(
      Expression{MemberAccessExpression{std::move(object), name.data()}}));
}

class StmtBuilder {
 public:
  std::unique_ptr<Statement> node;

  explicit StmtBuilder(std::unique_ptr<Statement> n) : node(std::move(n)) {}

  StmtBuilder(const StmtBuilder&) = delete;
  StmtBuilder& operator=(const StmtBuilder&) = delete;

  StmtBuilder(StmtBuilder&&) noexcept = default;
  StmtBuilder& operator=(StmtBuilder&&) noexcept = default;

  operator std::unique_ptr<Statement>() { return std::move(node); }
};

StmtBuilder expr(std::unique_ptr<Expression> e) {
  return StmtBuilder(std::make_unique<Statement>(Statement{std::move(e)}));
}

StmtBuilder let(std::string name, ExprBuilder init) {
  return StmtBuilder(std::make_unique<Statement>(Statement{
      AssignStatement{std::move(name), std::nullopt, std::move(init.node)}}));
}

StmtBuilder let(std::string name, ParsedType type, ExprBuilder init) {
  return StmtBuilder(std::make_unique<Statement>(Statement{AssignStatement{
      std::move(name), std::move(type), std::move(init.node)}}));
}

template <typename... Stmts>
Block block(Stmts&&... stmts) {
  Block b;
  b.statements.reserve(sizeof...(stmts));
  (b.statements.push_back(std::move(stmts)), ...);
  return b;
}

}  // namespace testing