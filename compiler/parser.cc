// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/parser.h"

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "compiler/tokenizer.h"
#include "compiler/types.h"

namespace {

template <typename T>
std::unique_ptr<Expression> make_primary_expression(T&& value,
                                                    const Token& token) {
  return std::make_unique<Expression>(
      Expression{PrimaryExpression{std::forward<T>(value)}, token.meta});
}

struct EscapeResult {
  uint32_t codepoint;
  size_t bytes_consumed;
  std::optional<std::string> error_message = std::nullopt;
};

EscapeResult unescape_codepoint(std::string_view input) {
  if (input.empty()) {
    return {0, 0, "incomplete escape sequence"};
  }

  char ch = input[0];

  // Normal character
  if (ch != '\\') {
    return {static_cast<uint8_t>(ch), 1};
  }

  // Escape must have at least 2 chars
  if (input.size() < 2) {
    return {0, 1, "incomplete escape sequence"};
  }

  char esc = input[1];
  switch (esc) {
    case 'a':
      return {'\a', 2};
    case 'b':
      return {'\b', 2};
    case 'f':
      return {'\f', 2};
    case 'n':
      return {'\n', 2};
    case 'r':
      return {'\r', 2};
    case 't':
      return {'\t', 2};
    case 'v':
      return {'\v', 2};
    case '\\':
      return {'\\', 2};
    case '\'':
      return {'\'', 2};
    case '"':
      return {'"', 2};
    default:
      return {0, 2, "invalid escape sequence"};
  }
}

}  // namespace

Block Parser::Parse() {
  Block root_block;
  ParseBlock(root_block, BlockType::Root);
  return root_block;
}

#define ASSIGN_OR_CONTINUE($var, $expr) \
  auto $var = ($expr);                  \
  if (!$var) {                          \
    continue;                           \
  }
#define CONTINUE_IF_FALSE($expr) \
  if (!($expr)) {                \
    continue;                    \
  }

void Parser::ParseBlock(Block& block, BlockType block_type) {
  while (current_token_.kind != TokenKind::kEndOfFile &&
         current_token_.kind != TokenKind::kCloseBrace) {
    Token start_token = current_token_;

    bool is_extern = false;
    Metadata extern_metadata = current_token_.meta;
    if (current_token_.kind == TokenKind::kKwExtern) {
      is_extern = true;
      AdvanceToken();
    }

    // Function definition i.e. fn $name($args,...):
    if (current_token_.kind == TokenKind::kKwFn) {
      FunctionKind parse_function_as =
          is_extern ? FunctionKind::Extern : FunctionKind::Free;
      if (auto fn = ParseFunctionDeclaration(parse_function_as)) {
        block.statements.push_back(std::make_unique<Statement>(
            Statement{std::move(*fn),
                      Metadata::fromTokens(start_token, current_token_)}));
      }
      continue;
    }

    // struct definition i.e. struct $name { $field: $type, ... }
    if (current_token_.kind == TokenKind::kKwStruct) {
      if (auto struct_decl = ParseStructDeclaration(
              is_extern ? ExternStruct::YES : ExternStruct::NO)) {
        block.statements.push_back(std::make_unique<Statement>(
            Statement{std::move(*struct_decl),
                      Metadata::fromTokens(start_token, current_token_)}));
      }
      continue;
    }

    if (is_extern) {
      error_collector_.Add("extern is only valid before struct/fn",
                           extern_metadata);
      SynchronizeOnError();  // Sync to start of next statement
      continue;
    }

    if (current_token_.kind == TokenKind::kKwWhile) {
      AdvanceToken();

      auto condition_expression = ParseBlockCondition();

      if (!ConsumeToken(TokenKind::kOpenBrace,
                        "expected '{' before while body")) {
        SynchronizeOnError(
            [](TokenKind kind) { return kind == TokenKind::kCloseBrace; });
        // If we hit '}' do not consume it since there was never '{'
        continue;
      }

      WhileStatement while_stmt{std::move(condition_expression), Block{}};
      ParseBlock(while_stmt.body);

      block.statements.push_back(std::make_unique<Statement>(
          Statement{std::move(while_stmt),
                    Metadata::fromTokens(start_token, current_token_)}));
      continue;
    }

    if (current_token_.kind == TokenKind::kKwIf) {
      AdvanceToken();

      auto condition_expression = ParseBlockCondition();

      if (!ConsumeToken(TokenKind::kOpenBrace, "expected '{' before if body")) {
        SynchronizeOnError(
            [](TokenKind kind) { return kind == TokenKind::kCloseBrace; });
        // If we hit '}' do not consume it since there was never '{'
        continue;
      }

      IfStatement if_stmt{std::move(condition_expression), Block{}, Block{}};

      ParseBlock(if_stmt.then_body);

      // Optional else block
      if (current_token_.kind == TokenKind::kKwElse) {
        AdvanceToken();

        if (!ConsumeToken(TokenKind::kOpenBrace,
                          "expected '{' before if body")) {
          SynchronizeOnError(
              [](TokenKind kind) { return kind == TokenKind::kCloseBrace; });
          // If we hit '}' do not consume it since there was never '{'
          continue;
        }

        ParseBlock(if_stmt.else_body);
      }

      block.statements.push_back(std::make_unique<Statement>(
          Statement{std::move(if_stmt),
                    Metadata::fromTokens(start_token, current_token_)}));
      continue;
    }

    if (current_token_.kind == TokenKind::kKwReturn) {
      AdvanceToken();  // consume 'return'

      if (auto expr = ParseExpression()) {
        if (!ConsumeToken(TokenKind::kEndExpr,
                          "expected ';' after expression")) {
          SynchronizeOnError();
          // Intentional fall-through as an expression was parsed
        }

        block.statements.push_back(std::make_unique<Statement>(
            Statement{ReturnStatement{std::move(expr)},
                      Metadata::fromTokens(start_token, current_token_)}));
      }
      continue;
    }

    if (current_token_.kind == TokenKind::kKwThrow) {
      AdvanceToken();  // consume 'throw'

      if (auto expr = ParseExpression()) {
        if (!ConsumeToken(TokenKind::kEndExpr,
                          "expected ';' after expression")) {
          SynchronizeOnError();
          // Intentional fall-through as an expression was parsed
        }
        block.statements.push_back(std::make_unique<Statement>(
            Statement{ThrowStatement{std::move(expr)},
                      Metadata::fromTokens(start_token, current_token_)}));
      }
      continue;
    }

    if (current_token_.kind == TokenKind::kKwBreak) {
      AdvanceToken();  // consume 'break'

      if (!ConsumeToken(TokenKind::kEndExpr, "expected ';' after break")) {
        SynchronizeOnError();
        // Intentional fall-through as an expression was parsed
      }

      block.statements.push_back(std::make_unique<Statement>(
          Statement{BreakStatement{},
                    Metadata::fromTokens(start_token, current_token_)}));
      continue;
    }

    if (current_token_.kind == TokenKind::kKwContinue) {
      AdvanceToken();  // consume 'continue'

      if (!ConsumeToken(TokenKind::kEndExpr, "expected ';' after continue")) {
        SynchronizeOnError();
        // Intentional fall-through as an expression was parsed
      }

      block.statements.push_back(std::make_unique<Statement>(
          Statement{ContinueStatement{},
                    Metadata::fromTokens(start_token, current_token_)}));
      continue;
    }

    if (current_token_.kind == TokenKind::kKwLet) {
      AdvanceToken();  // consume `let`

      Token variable_name = current_token_;
      if (!ConsumeToken(TokenKind::kIdent, "expected variable name")) {
        SynchronizeOnError();
        continue;
      }

      std::optional<ParsedType> var_type;
      // : <type> is optional, but if there is a ':' there must be a type.
      if (current_token_.kind == TokenKind::kColon) {
        AdvanceToken();  // after ':'

        Metadata type_metadata = current_token_.meta;
        var_type = ParseType();
        if (!var_type.has_value()) {
          error_collector_.Add("expected well formed type after ':'",
                               type_metadata);
          // Fallthrough to attempt to keep parsing as types are optional
        }
      }

      // If the variable isn't initialized this was likely a programmer error
      // and not a typo so there's not saving this `let` statement.
      if (!ConsumeToken(TokenKind::kAssign,
                        "expected assign (=) operator; all variables MUST be "
                        "initialized")) {
        SynchronizeOnError();
        continue;
      }

      std::unique_ptr<Expression> value_expression = ParseExpression();
      if (!value_expression) {
        // ParseExpression likely logged its own error inside, sync and drop out
        SynchronizeOnError();
        continue;
      }

      if (!ConsumeToken(TokenKind::kEndExpr, "expected ';' after expression")) {
        SynchronizeOnError();
        // Intentional fall-through as an expression was parsed
      }

      block.statements.push_back(std::make_unique<Statement>(Statement{
          AssignStatement{SpannedText::FromToken(std::move(variable_name)),
                          std::move(var_type), std::move(value_expression)},
          Metadata::fromTokens(start_token, current_token_)}));
      continue;
    }

    if (current_token_.kind == TokenKind::kKwAlias) {
      AdvanceToken();  // Past "alias"

      if (current_token_.kind != TokenKind::kIdent) {
        error_collector_.Add("expected type alias name after 'alias'",
                             current_token_.meta);
        SynchronizeOnError();
        continue;
      }

      Token name_token = current_token_;
      AdvanceToken();  // consume name

      if (!ConsumeToken(TokenKind::kAssign,
                        "expected '=' after type alias name")) {
        if (!SynchronizeOnError(
                [](TokenKind kind) { return kind == TokenKind::kAssign; })) {
          continue;
        }

        AdvanceToken();  // consume =
        // Intentional fall-through to try to continue parsing
      }

      std::optional<ParsedType> type = ParseType();
      if (!type.has_value()) {
        error_collector_.Add("expected valid type after '='",
                             current_token_.meta);
        SynchronizeOnError();
        continue;
      }

      if (!ConsumeToken(TokenKind::kEndExpr,
                        "expected ';' to terminate type alias statement")) {
        if (SynchronizeOnError(
                [](TokenKind kind) { return kind == TokenKind::kEndExpr; })) {
          AdvanceToken();  // consume ;
        }
        // Intentional fall-through as the expression is fully parsed
      }

      block.statements.push_back(std::make_unique<Statement>(
          Statement{TypeAliasStatement{
                        SpannedText::FromToken(std::move(name_token)),
                        std::make_unique<ParsedType>(std::move(type.value()))},
                    Metadata::fromTokens(start_token, current_token_)}));
      continue;
    }

    if (current_token_.kind == TokenKind::kKwImport) {
      Token import_token = current_token_;
      AdvanceToken();  // Past @import

      if (block_type != BlockType::Root) {
        error_collector_.Add("@imports can only appear in the root scope",
                             import_token.meta);
        SynchronizeOnError();
        continue;
      }

      if (current_token_.kind != TokenKind::kString) {
        error_collector_.Add("@import missing \"path/to/import\"",
                             current_token_.meta);
        if (!SynchronizeOnError(
                [](TokenKind kind) { return kind == TokenKind::kString; })) {
          continue;
        }
        // `current_token_` is a String!
      }

      Token import_path = current_token_;
      AdvanceToken();  // Past @import "path/to/import"

      if (current_token_.kind == TokenKind::kEndExpr) {
        AdvanceToken();  // Skip past ; but do not consider it missing an error
      }

      block.statements.push_back(std::make_unique<Statement>(
          Statement{ImportStatement{std::move(import_path.value)}}));
      continue;
    }

    if (auto expr = ParseExpression()) {
      Token end_token = current_token_;
      if (!ConsumeToken(TokenKind::kEndExpr,
                        "expected ';' to terminate expression statement")) {
        if (SynchronizeOnError(
                [](TokenKind kind) { return kind == TokenKind::kEndExpr; })) {
          end_token = current_token_;
          AdvanceToken();  // consume the ';' we synchronized to
        }
        // Intentional fall-through as the expression is fully parsed
      }
      block.statements.push_back(std::make_unique<Statement>(Statement{
          std::move(expr), Metadata::fromTokens(start_token, end_token)}));
      continue;
    }

    error_collector_.Add("unexpected token", current_token_.meta);
    AdvanceToken();
  }

  AdvanceToken();  // consume '}' or EOF
}

std::unique_ptr<Expression> Parser::ParseValue() {
  switch (current_token_.kind) {
    case TokenKind::kNumber: {
      if ((current_token_.value.find('.') != std::string::npos) ||
          current_token_.value.back() == 'f') {
        float f32 = std::stof(current_token_.value);
        return make_primary_expression(f32, current_token_);
      } else {
        int i32 = std::stoi(current_token_.value);
        return make_primary_expression(i32, current_token_);
      }
    }
    case TokenKind::kIdent: {
      return make_primary_expression(Identifier{.name = current_token_.value},
                                     current_token_);
    }
    case TokenKind::kString: {
      size_t i = 0;
      std::string unescaped;
      while (i < current_token_.value.size()) {
        auto res = unescape_codepoint(current_token_.value.substr(i));

        if (res.error_message.has_value()) {
          error_collector_.Add(res.error_message.value(), current_token_.meta);
          return nullptr;
        }

        if (res.codepoint > 0x7F) {
          error_collector_.Add(
              "non-ASCII characters are not supported in string literals",
              current_token_.meta);
          return nullptr;
        }

        unescaped += static_cast<char>(res.codepoint);
        i += res.bytes_consumed;
      }

      return make_primary_expression(StringLiteral{unescaped}, current_token_);
    }
    case TokenKind::kChar: {
      auto res = unescape_codepoint(current_token_.value);
      if (res.error_message.has_value()) {
        error_collector_.Add(res.error_message.value(), current_token_.meta);
        return nullptr;
      }

      if (res.codepoint > 0x7F) {
        error_collector_.Add(
            "non-ASCII characters are not supported in char literals",
            current_token_.meta);
        return nullptr;
      }

      if (res.bytes_consumed != current_token_.value.size()) {
        error_collector_.Add("char literal is too long", current_token_.meta);
        return nullptr;
      }

      return make_primary_expression(
          CodepointLiteral{res.codepoint, current_token_.value},
          current_token_);
    }
    case TokenKind::kKwTrue: {
      return make_primary_expression(true, current_token_);
    }
    case TokenKind::kKwFalse: {
      return make_primary_expression(false, current_token_);
    }
    case TokenKind::kKwNil: {
      return make_primary_expression(Nil{}, current_token_);
    }
    default:
      error_collector_.Add("unknown literal type", current_token_.meta);
      return nullptr;
  }
}

std::unique_ptr<Expression> Parser::ParseBlockCondition() {
  auto is_start_of_body = [](TokenKind kind) {
    return kind == TokenKind::kOpenBrace;
  };

  if (!ConsumeToken(TokenKind::kOpenParen,
                    "expected '(' before while condition")) {
    SynchronizeOnError(is_start_of_body);
    return nullptr;
  }

  std::unique_ptr<Expression> condition_expression = ParseExpression();
  if (!condition_expression) {
    SynchronizeOnError(is_start_of_body);
    return nullptr;
  }

  if (!ConsumeToken(TokenKind::kCloseParen,
                    "expected ')' after while condition")) {
    SynchronizeOnError(is_start_of_body);
    // Intentional fall-through to preserve the parsed condition
  }

  return condition_expression;
}

std::unique_ptr<Expression> Parser::ParseExpression() {
  return ParseAssignment();
}

std::unique_ptr<Expression> Parser::ParseAssignment() {
  Token start_token = current_token_;
  auto expr = ParseLogical();

  if (expr && current_token_.kind == TokenKind::kAssign) {
    Token op = current_token_;
    AdvanceToken();  // consume '='

    auto rhs = ParseExpression();
    if (!rhs)
      return nullptr;

    return std::make_unique<Expression>(
        Expression{AssignmentExpression{std::move(expr), std::move(rhs)},
                   Metadata::fromTokens(start_token, current_token_)});
  }

  return expr;
}

std::unique_ptr<Expression> Parser::ParseLogical() {
  Token start_token = current_token_;

  auto lhs = ParseComparison();
  if (!lhs)
    return nullptr;

  while (current_token_.kind == TokenKind::kAndAnd ||
         current_token_.kind == TokenKind::kOrOr) {
    Token op = current_token_;
    AdvanceToken();

    auto rhs = ParseComparison();
    if (!rhs)
      return nullptr;

    lhs = std::make_unique<Expression>(Expression{
        LogicExpression{op.kind == TokenKind::kAndAnd ? LogicExpression::AND
                                                      : LogicExpression::OR,
                        std::move(lhs), std::move(rhs)},
        Metadata::fromTokens(start_token, current_token_)});
  }
  return lhs;
}

std::unique_ptr<Expression> Parser::ParseComparison() {
  Token start_token = current_token_;

  auto lhs = ParseAdditive();
  if (!lhs)
    return nullptr;

  while (current_token_.kind == TokenKind::kCompareEq ||
         current_token_.kind == TokenKind::kCompareNe ||
         current_token_.kind == TokenKind::kCompareLt ||
         current_token_.kind == TokenKind::kCompareLe ||
         current_token_.kind == TokenKind::kCompareGt ||
         current_token_.kind == TokenKind::kCompareGe) {
    Token op = current_token_;
    AdvanceToken();

    auto rhs = ParseAdditive();
    if (!rhs)
      return nullptr;

    lhs = std::make_unique<Expression>(
        Expression{BinaryExpression{op.kind, std::move(lhs), std::move(rhs)},
                   Metadata::fromTokens(start_token, current_token_)});
  }
  return lhs;
}

std::unique_ptr<Expression> Parser::ParseAdditive() {
  Token start_token = current_token_;

  auto lhs = ParseMultiplicative();
  if (!lhs)
    return nullptr;

  while (current_token_.kind == TokenKind::kPlus ||
         current_token_.kind == TokenKind::kMinus) {
    Token op = current_token_;
    AdvanceToken();

    auto rhs = ParseMultiplicative();
    if (!rhs)
      return nullptr;

    lhs = std::make_unique<Expression>(
        Expression{BinaryExpression{op.kind, std::move(lhs), std::move(rhs)},
                   Metadata::fromTokens(start_token, current_token_)});
  }

  return lhs;
}

std::unique_ptr<Expression> Parser::ParseMultiplicative() {
  Token start_token = current_token_;

  auto lhs = ParseUnary();
  if (!lhs)
    return nullptr;

  while (current_token_.kind == TokenKind::kMultiply ||
         current_token_.kind == TokenKind::kDivide) {
    Token op = current_token_;
    AdvanceToken();

    auto rhs = ParseUnary();
    if (!rhs)
      return nullptr;

    lhs = std::make_unique<Expression>(
        Expression{BinaryExpression{op.kind, std::move(lhs), std::move(rhs)},
                   Metadata::fromTokens(start_token, current_token_)});
  }

  return lhs;
}

std::unique_ptr<Expression> Parser::ParseUnary() {
  Token start_token = current_token_;

  if (current_token_.kind == TokenKind::kPlus ||
      current_token_.kind == TokenKind::kMinus ||
      current_token_.kind == TokenKind::kPlusPlus ||
      current_token_.kind == TokenKind::kMinusMinus ||
      current_token_.kind == TokenKind::kNot) {
    Token op = current_token_;
    AdvanceToken();  // consume operator

    auto operand = ParseUnary();
    if (!operand)
      return nullptr;

    return std::make_unique<Expression>(
        Expression{PrefixUnaryExpression{op.kind, std::move(operand)},
                   Metadata::fromTokens(start_token, current_token_)});
  }

  return ParseInFix();
}

std::unique_ptr<Expression> Parser::ParseInFix() {
  auto expr = ParsePostFix();
  if (!expr)
    return nullptr;

  while (true) {
    if (current_token_.kind == TokenKind::kKwAs) {
      Token start_token = current_token_;
      AdvanceToken();  // consume 'as'

      bool should_allow_as_nil = false;
      if (current_token_.kind == TokenKind::kQuestion) {
        AdvanceToken();  // consume '?'
        should_allow_as_nil = true;
      }

      std::optional<ParsedType> type = ParseType();
      if (!type)
        return nullptr;

      expr = std::make_unique<Expression>(Expression{
          TypeCastExpression{std::move(expr), std::move(type.value()),
                             should_allow_as_nil ? TypeCastStrategy::OPTIONAL
                                                 : TypeCastStrategy::STRICT},
          Metadata::fromTokens(start_token, current_token_)});
      continue;
    }

    if (current_token_.kind == TokenKind::kQuestionQuestion) {
      Token start_token = current_token_;
      AdvanceToken();  // consume '??'

      // Allows for right associativity in expressions like `x ?? y ?? 0`.
      std::unique_ptr<Expression> rhs = ParseInFix();
      if (!rhs)
        return nullptr;

      expr = std::make_unique<Expression>(
          Expression{NilCoalescingExpression{std::move(expr), std::move(rhs)},
                     Metadata::fromTokens(start_token, current_token_)});
      continue;
    }
    break;
  }

  return expr;
}

std::unique_ptr<Expression> Parser::ParsePostFix() {
  Token start_token = current_token_;

  auto expr = ParsePrimary();
  if (!expr)
    return nullptr;

  bool is_optional_chain = false;
  while (true) {
    Token post_fix_start_token = current_token_;

    std::optional<Metadata> optional_access_metadata;
    if (current_token_.kind == TokenKind::kQuestion) {
      optional_access_metadata = current_token_.meta;
      AdvanceToken();  // consume '?'
      is_optional_chain = true;
    }

    auto wrap_optional = [&](std::unique_ptr<Expression> expr) {
      return optional_access_metadata.has_value()
                 ? std::make_unique<Expression>(
                       Expression{OptionalAccessExpression{std::move(expr)}})
                 : std::move(expr);
    };

    if (current_token_.kind == TokenKind::kDot) {
      AdvanceToken();  // consume '.'

      if (current_token_.kind == TokenKind::kKwTypeOf) {
        AdvanceToken();  // consume 'of'

        if (!ConsumeToken(TokenKind::kOpenParen, "expect template arguments")) {
          if (!SynchronizeOnError([](TokenKind kind) {
                return kind == TokenKind::kCloseParen ||
                       kind == TokenKind::kEndExpr;
              })) {
            return nullptr;
          }
          if (current_token_.kind == TokenKind::kCloseParen)
            AdvanceToken();
          continue;
        }

        std::vector<ParsedType> template_types =
            ParseTypeList(TokenKind::kCloseParen);

        if (template_types.empty())
          return nullptr;

        expr = std::make_unique<Expression>(Expression{
            TemplateInstantiationExpression{std::move(expr),
                                            std::move(template_types)},
            Metadata::fromTokens(post_fix_start_token, current_token_)});
        continue;
      }

      if (current_token_.kind != TokenKind::kIdent) {
        error_collector_.Add("expected identifier after '.'",
                             current_token_.meta);
        if (!SynchronizeOnError([](TokenKind k) {
              return k == TokenKind::kDot || k == TokenKind::kOpenParen ||
                     k == TokenKind::kSquareOpen;
            })) {
          return nullptr;
        }
        continue;
      }
      Token ident = current_token_;
      AdvanceToken();  // consume identifier

      expr = std::make_unique<Expression>(Expression{
          MemberAccessExpression{wrap_optional(std::move(expr)), ident.value},
          Metadata::fromTokens(post_fix_start_token, current_token_)});
      continue;
    }

    // Handle function call i.e. $expr(...)
    if (current_token_.kind == TokenKind::kOpenParen) {
      expr = ParseCall(wrap_optional(std::move(expr)));
      if (!expr)
        return nullptr;
      continue;
    }

    if (current_token_.kind == TokenKind::kSquareOpen) {
      AdvanceToken();  // consume '['

      auto index = ParseExpression();
      if (!index) {
        if (!SynchronizeOnError([](TokenKind kind) {
              return kind == TokenKind::kSquareClose;
            })) {
          return nullptr;
        }
        if (current_token_.kind == TokenKind::kSquareClose)
          AdvanceToken();
        continue;
      }

      if (!ConsumeToken(TokenKind::kSquareClose,
                        "expected ']' to close array access")) {
        if (!SynchronizeOnError(
                [](TokenKind k) { return k == TokenKind::kSquareClose; })) {
          return nullptr;
        }
      }

      if (current_token_.kind == TokenKind::kSquareClose)
        AdvanceToken();  // consume ']'

      expr = std::make_unique<Expression>(Expression{
          ArrayAccessExpression{wrap_optional(std::move(expr)),
                                std::move(index)},
          Metadata::fromTokens(post_fix_start_token, current_token_)});
      continue;
    }

    if (optional_access_metadata.has_value()) {
      error_collector_.Add(
          "optional chaining is only supported for member access (?.), "
          "subscript (?[]), and function call fn?()",
          *optional_access_metadata);
      continue;
    }

    if (current_token_.kind == TokenKind::kPlusPlus ||
        current_token_.kind == TokenKind::kMinusMinus) {
      Token op = current_token_;
      AdvanceToken();  // consume operator
      expr = std::make_unique<Expression>(Expression{
          PostfixUnaryExpression{op.kind, std::move(expr)},
          Metadata::fromTokens(post_fix_start_token, current_token_)});
      continue;
    }
    break;
  }

  if (is_optional_chain) {
    expr = std::make_unique<Expression>(
        Expression{OptionalChainExpression{std::move(expr)},
                   Metadata::fromTokens(start_token, current_token_)});
  }
  return expr;
}

std::unique_ptr<Expression> Parser::ParsePrimary() {
  switch (current_token_.kind) {
    case TokenKind::kNumber:
    case TokenKind::kString:
    case TokenKind::kChar:
    case TokenKind::kIdent:
    case TokenKind::kKwTrue:
    case TokenKind::kKwFalse:
    case TokenKind::kKwNil: {
      auto expr = ParseValue();
      AdvanceToken();
      return expr;
    }

    case TokenKind::kOpenParen: {
      AdvanceToken();  // consume '('

      auto expr = ParseExpression();
      if (!expr) {
        if (!SynchronizeOnError([](TokenKind kind) {
              return kind == TokenKind::kCloseParen;
            })) {
          return nullptr;
        }
      }

      if (!ConsumeToken(TokenKind::kCloseParen, "expected ')'")) {
        if (SynchronizeOnError([](TokenKind kind) {
              return kind == TokenKind::kCloseParen;
            })) {
          AdvanceToken();  // consume ')' since we synchronized to anchor
        } else {
          return nullptr;  // synchronized to top-level statement abort
        }
      }
      return expr;
    }

    case TokenKind::kKwFn: {
      auto start_token = current_token_;
      if (auto fn = ParseFunctionDeclaration(FunctionKind::Anonymous)) {
        return std::make_unique<Expression>(
            Expression{ClosureExpression{std::move(*fn)},
                       Metadata::fromTokens(start_token, current_token_)});
      }
      return nullptr;
    }

    default:
      error_collector_.Add("expected expression", current_token_.meta);
      return nullptr;
  }
}

std::unique_ptr<Expression> Parser::ParseCall(
    std::unique_ptr<Expression> callee) {
  Token start_token = current_token_;
  AdvanceToken();  // consume '('

  std::vector<std::unique_ptr<Expression>> arguments;
  if (current_token_.kind != TokenKind::kCloseParen) {
    while (current_token_.kind != TokenKind::kCloseParen) {
      if (auto arg = ParseExpression()) {
        arguments.push_back(std::move(arg));
      } else {
        if (!SynchronizeOnError([](TokenKind kind) {
              return kind == TokenKind::kComma ||
                     kind == TokenKind::kCloseParen;
            })) {
          return nullptr;
        }

        if (current_token_.kind == TokenKind::kComma)
          AdvanceToken();
        continue;
      }

      if (current_token_.kind == TokenKind::kComma) {
        AdvanceToken();  // consume ','
      } else if (current_token_.kind != TokenKind::kCloseParen) {
        error_collector_.Add("expected ',' or ')' after argument",
                             current_token_.meta);

        if (!SynchronizeOnError([](TokenKind kind) {
              return kind == TokenKind::kComma ||
                     kind == TokenKind::kCloseParen;
            })) {
          return nullptr;
        }

        if (current_token_.kind == TokenKind::kComma)
          AdvanceToken();
      }
    }
  }

  CHECK(
      ConsumeToken(TokenKind::kCloseParen, "expected ')' to close arguments"));

  return std::make_unique<Expression>(
      Expression{CallExpression{std::move(callee), std::move(arguments)},
                 Metadata::fromTokens(start_token, current_token_)});
}

std::optional<ParsedType> Parser::ParseType() {
  return ParseUnionType();
}

std::optional<ParsedType> Parser::ParseUnionType() {
  Token start_token = current_token_;

  std::vector<ParsedType> types;

  auto type = ParsePrimaryType();
  if (!type)
    return std::nullopt;

  types.push_back(std::move(type.value()));

  while (current_token_.kind == TokenKind::kPipe) {
    AdvanceToken();  // consume '|'

    auto next = ParsePrimaryType();
    if (!next)
      return std::nullopt;

    types.push_back(std::move(next.value()));
  }

  if (types.size() == 1)
    return types.front();

  return ParsedType{ParsedUnionType{std::move(types)},
                    Metadata::fromTokens(start_token, current_token_)};
}

std::optional<ParsedType> Parser::ParseFunctionType() {
  Token start_token = current_token_;
  CHECK(ConsumeToken(TokenKind::kKwFn, "expected 'fn'"));

  if (!ConsumeToken(TokenKind::kOpenParen, "expected '('"))
    return std::nullopt;

  std::vector<ParsedType> arg_types;
  if (current_token_.kind != TokenKind::kCloseParen) {
    while (true) {
      auto arg_type = ParseType();
      if (!arg_type) {
        if (!SynchronizeOnError([](TokenKind kind) {
              return kind == TokenKind::kComma ||
                     kind == TokenKind::kCloseParen;
            })) {
          return std::nullopt;
        }
      } else {
        arg_types.push_back(std::move(arg_type.value()));
      }

      if (current_token_.kind == TokenKind::kComma) {
        AdvanceToken();  // consume ','
        continue;
      }

      if (current_token_.kind != TokenKind::kCloseParen) {
        if (!SynchronizeOnError([](TokenKind kind) {
              return kind == TokenKind::kCloseParen ||
                     kind == TokenKind::kSkinnyArrow;
            })) {
          return std::nullopt;
        }
      }
      break;
    }
  }

  if (current_token_.kind == TokenKind::kCloseParen)
    AdvanceToken();  // consume ')'

  std::shared_ptr<ParsedType> return_type;
  if (current_token_.kind == TokenKind::kSkinnyArrow) {
    AdvanceToken();  // consume '->'

    auto parsed_return = ParseType();
    if (!parsed_return)
      return std::nullopt;

    return_type =
        std::make_shared<ParsedType>(std::move(parsed_return.value()));
  }

  return ParsedType{
      ParsedFunctionType{std::move(arg_types), std::move(return_type)},
      Metadata::fromTokens(start_token, current_token_)};
}

std::optional<ParsedType> Parser::ParsePrimaryType() {
  Token start_token = current_token_;

  ParsedType result;
  // Handles parenthesized types i.e. (fn() -> i32) or (String | i32)?.
  if (current_token_.kind == TokenKind::kOpenParen) {
    AdvanceToken();  // consume '('

    auto inner_type = ParseType();
    if (!inner_type)
      return std::nullopt;

    ConsumeToken(TokenKind::kCloseParen, "expected ')'");
    result = std::move(inner_type.value());
  }
  // Handles function types i.e. fn(i32) -> bool.
  else if (current_token_.kind == TokenKind::kKwFn) {
    auto func_type = ParseFunctionType();
    if (!func_type)
      return std::nullopt;
    result = std::move(func_type.value());
  }
  // Handles type atoms i.e. String, i32, MyStruct, and Nil.
  // Nil will NEVER be parameterized as a generic but neither will i32...
  // validation will be done in SemanticAnalyzer not here.
  else if (current_token_.kind == TokenKind::kIdent ||
           current_token_.kind == TokenKind::kKwNil) {
    result = ParsedType{current_token_.value, current_token_.meta};
    AdvanceToken();  // consume identifer (or Nil)

    if (current_token_.kind == TokenKind::kSquareOpen) {
      AdvanceToken();  // consume [

      std::vector<ParsedType> parameters =
          ParseTypeList(TokenKind::kSquareClose);

      if (parameters.empty())
        return std::nullopt;

      result = ParsedType{ParsedParameterizedType{
                              std::make_shared<ParsedType>(std::move(result)),
                              std::move(parameters)},
                          Metadata::fromTokens(start_token, current_token_)};
    }
  } else {
    return std::nullopt;
  }

  // Handles wrapping types as an Optional i.e. String?, (fn() -> i32)?, etc.
  if (current_token_.kind == TokenKind::kQuestion) {
    AdvanceToken();  // consume '?'
    result = ParsedType{
        ParsedOptionalType{std::make_shared<ParsedType>(std::move(result))},
        Metadata::fromTokens(start_token, current_token_)};
  }

  return result;
}

std::vector<ParsedType> Parser::ParseTypeList(TokenKind end_of_list_token) {
  std::vector<ParsedType> return_types;
  while (true) {
    auto type = ParseType();
    if (!type)
      return {};

    return_types.push_back(std::move(type.value()));

    if (current_token_.kind == TokenKind::kComma) {
      AdvanceToken();  // consume ,
      continue;
    }

    if (current_token_.kind == end_of_list_token) {
      AdvanceToken();  // consume `end_of_list_token`
      break;
    }
  }
  return return_types;
}

std::optional<std::vector<TemplateArgument>>
Parser::ParseTemplateDeclarationList() {
  CHECK_EQ(current_token_.kind, TokenKind::kSquareOpen);
  AdvanceToken();  // consume [

  std::vector<TemplateArgument> template_parameters;
  while (current_token_.kind != TokenKind::kSquareClose &&
         current_token_.kind != TokenKind::kEndOfFile) {
    Token parameter_token = current_token_;
    if (!ConsumeToken(TokenKind::kIdent, "expected template parameter")) {
      // Attempt to synchronize to the next parameter (or end of list)

      if (!SynchronizeOnError([](TokenKind kind) {
            return kind == TokenKind::kComma || kind == TokenKind::kSquareClose;
          })) {
        return std::nullopt;
      }
      if (current_token_.kind == TokenKind::kComma)
        AdvanceToken();
      continue;
    }

    std::optional<ParsedType> default_type;  // Default types are optional
    if (current_token_.kind == TokenKind::kAssign) {
      AdvanceToken();  // consume =
      default_type = ParseType();

      if (!default_type.has_value()) {
        error_collector_.Add(
            "expected valid type after '=' in template parameter",
            current_token_.meta);
        // Intentional fall-through to continue parsing since optional
      }
    }

    template_parameters.emplace_back(
        SpannedText::FromToken(std::move(parameter_token)),
        std::move(default_type));

    if (current_token_.kind == TokenKind::kComma) {
      AdvanceToken();  // consume ,
    } else if (current_token_.kind != TokenKind::kSquareClose) {
      error_collector_.Add("expected ',' or ']' after template parameter",
                           current_token_.meta);
      if (!SynchronizeOnError(
              [](TokenKind kind) { return kind == TokenKind::kSquareClose; })) {
        return std::nullopt;
      }
    }
  }

  ConsumeToken(TokenKind::kSquareClose,
               "expected ']' to close template arguments");
  return template_parameters;
}

std::optional<StructDeclaration> Parser::ParseStructDeclaration(
    ExternStruct is_extern) {
  CHECK(ConsumeToken(TokenKind::kKwStruct, "expected 'struct'"));

  Token name_token = current_token_;
  if (!ConsumeToken(TokenKind::kIdent, "requires a struct name")) {
    error_collector_.Add("requires a struct name", current_token_.meta);

    // Synchronize to the start of template parameters or body
    auto is_body_or_templates = [](TokenKind kind) {
      return kind == TokenKind::kSquareOpen || kind == TokenKind::kOpenBrace;
    };
    if (!SynchronizeOnError(is_body_or_templates)) {
      return std::nullopt;
    }
  }

  // Optionally parse template paramters i.e. [T, U = i32]
  std::vector<TemplateArgument> template_parameters;
  if (current_token_.kind == TokenKind::kSquareOpen) {
    auto result = ParseTemplateDeclarationList();
    if (!result)
      return std::nullopt;
    template_parameters = std::move(result.value());
  }

  StructDeclaration struct_decl;
  struct_decl.name = SpannedText::FromToken(std::move(name_token));
  struct_decl.is_extern = is_extern == ExternStruct::YES;
  struct_decl.template_arguments = std::move(template_parameters);

  if (!ConsumeToken(TokenKind::kOpenBrace, "expected '{' for struct body")) {
    return std::nullopt;
  }

  // Strong synchronization points to anchor on in a struct declaration
  auto is_strong_anchor = [](TokenKind kind) {
    return kind == TokenKind::kKwFn || kind == TokenKind::kKwStatic ||
           kind == TokenKind::kCloseBrace;
  };

  auto handle_member_error = [&, this]() -> bool {
    return SynchronizeOnError([&, this](TokenKind kind) {
      return is_strong_anchor(kind) || kind == TokenKind::kIdent;
    });
  };

  while (current_token_.kind != TokenKind::kCloseBrace &&
         current_token_.kind != TokenKind::kEndOfFile) {
    std::optional<Token> static_modifier_token;
    if (current_token_.kind == TokenKind::kKwStatic) {
      static_modifier_token = current_token_;
      AdvanceToken();
    }

    if (current_token_.kind == TokenKind::kKwFn) {
      auto method = ParseFunctionDeclaration(static_modifier_token.has_value()
                                                 ? FunctionKind::StaticMethod
                                                 : FunctionKind::Method);
      if (!method) {
        if (!handle_member_error())
          return std::nullopt;
        continue;
      }

      struct_decl.methods.emplace_back(method->name, std::move(*method));
      continue;
    }

    if (static_modifier_token) {
      error_collector_.Add("'static' is only valid on method declarations",
                           static_modifier_token->meta);
      // This line is fundamentally borked at this point so try to sync past it.
      if (!SynchronizeOnError([&](TokenKind kind) {
            return kind == TokenKind::kEndExpr || is_strong_anchor(kind);
          })) {
        return std::nullopt;
      }
      if (current_token_.kind == TokenKind::kEndExpr)
        AdvanceToken();  // consume ';'
      continue;
    }

    if (current_token_.kind == TokenKind::kIdent) {
      Token field_name = current_token_;
      AdvanceToken();  // consume name

      if (!ConsumeToken(TokenKind::kColon, "expected ':' after field name")) {
        if (!handle_member_error())
          return std::nullopt;
        continue;
      }

      std::optional<ParsedType> type = ParseType();
      if (!type.has_value()) {
        error_collector_.Add("expected type name for field",
                             current_token_.meta);
        if (!handle_member_error())
          return std::nullopt;
        continue;
      }

      struct_decl.fields.emplace_back(
          SpannedText::FromToken(std::move(field_name)), type.value());

      if (!ConsumeToken(TokenKind::kEndExpr,
                        "expected ';' after field declaration")) {
        if (!handle_member_error())
          return std::nullopt;
      }
      continue;
    }

    // Dead-end fallthrough fallback
    error_collector_.Add("invalid struct member declaration",
                         current_token_.meta);
    if (!handle_member_error())
      return std::nullopt;
  }

  ConsumeToken(TokenKind::kCloseBrace, "expected '}' to close struct body");
  return struct_decl;
}

std::optional<FunctionDeclaration> Parser::ParseFunctionDeclaration(
    FunctionKind function_kind) {
  Token fn_token = current_token_;
  CHECK(ConsumeToken(TokenKind::kKwFn, "expected 'fn'"));

  SpannedText function_name;
  std::vector<TemplateArgument> template_parameters;
  if (function_kind == FunctionKind::Anonymous) {
    // Perform single-token deletion recovery to try to keep parsing happy...
    if (current_token_.kind == TokenKind::kIdent) {
      error_collector_.Add("Anonymous functions should not have a name",
                           current_token_.meta);
      AdvanceToken();  // skip name
    }
    function_name = SpannedText{
        "__llamda." + std::to_string(anonymous_fn_counter_++), fn_token.meta};
  } else {
    Token name_token = current_token_;
    if (current_token_.kind != TokenKind::kIdent) {
      error_collector_.Add("requires a function name", current_token_.meta);

      // Synchronize to the start of the template parameters or arguments list
      auto is_arguments = [](TokenKind kind) {
        return kind == TokenKind::kSquareOpen || kind == TokenKind::kOpenParen;
      };
      if (!SynchronizeOnError(is_arguments)) {
        return std::nullopt;
      }
    } else {
      function_name = SpannedText::FromToken(std::move(name_token));
      AdvanceToken();  // after function name
    }

    // Optionally parse template paramters i.e. [T, U = i32]
    if (current_token_.kind == TokenKind::kSquareOpen) {
      auto result = ParseTemplateDeclarationList();
      if (!result)
        return std::nullopt;
      template_parameters = std::move(result.value());
    }
  }

  auto argument_list = ParseFunctionArgumentList();
  if (!argument_list) {
    return std::nullopt;  // Errors handled within `ParseFunctionArgumentList()`
  }

  // Used as the location where a return type _could_ be
  Metadata return_type_metadate = current_token_.meta;
  std::optional<ParsedType> return_type;
  if (current_token_.kind == TokenKind::kSkinnyArrow) {
    AdvanceToken();  // consume '->'
    return_type = ParseType();
  } else if (current_token_.kind == TokenKind::kIdent) {
    // The next token SHOULD be a '{' so if an ident is found instead it is a
    // pretty good guess that the user is intending to supply a return type.
    error_collector_.Add("expected '->' before return type specifier",
                         current_token_.meta);
    return_type = ParseType();
  } else {
    // Using the debug metadata for the last parentheses in the expression
    // ensures that errors with deduced "Void" point to the absence of a return.
    return_type = ParsedType{"Void", return_type_metadate};
  }

  if (!return_type.has_value()) {
    error_collector_.Add("parsed invalid return type", current_token_.meta);

    auto is_body_or_end = [](TokenKind kind) {
      return kind == TokenKind::kOpenBrace || kind == TokenKind::kEndExpr;
    };
    if (!SynchronizeOnError(is_body_or_end))
      return std::nullopt;
  }

  FunctionDeclaration fn{
      std::move(function_name),       std::move(argument_list->arguments),
      std::move(return_type.value()), function_kind,
      std::move(template_parameters), std::move(argument_list->variadic_type)};

  if (current_token_.kind == TokenKind::kEndExpr) {
    AdvanceToken();
    return fn;
  }

  // Avoid parsing the function body at all if there is no open brace. Any
  // statements that appear will count towards the outer scope -- not ideal but
  // better than proactively assuming a body and leaving things even more
  // unbalanced...
  if (!ConsumeToken(TokenKind::kOpenBrace,
                    "expected ';' (for extern function) or function body {}")) {
    return std::nullopt;
  }

  Block body;
  ParseBlock(body);  // ParseBlock() consumes '}'
  fn.body = std::make_unique<Block>(std::move(body));
  return std::move(fn);
}

std::optional<Parser::FunctionArgumentList>
Parser::ParseFunctionArgumentList() {
  if (!ConsumeToken(TokenKind::kOpenParen,
                    "expected '(' before function arguments")) {
    if (!SynchronizeOnError([](TokenKind kind) {
          return kind == TokenKind::kOpenBrace ||
                 kind == TokenKind::kSkinnyArrow;
        })) {
      return std::nullopt;
    }
    // We are still in the context of the function and can parse the return
    // type and/or the function body so return an empty arugment list.
    return Parser::FunctionArgumentList{};
  }

  std::vector<std::pair<SpannedText, ParsedType>> arguments;
  std::optional<VariadicType> variadic_type;

  Token start_of_arguments_token = current_token_;
  while (current_token_.kind != TokenKind::kCloseParen) {
    // extern functions support passing raw variadic arguments to the runtime.
    // This must be the very last parameter passed to the function.
    if (current_token_.kind == TokenKind::kVariadic) {
      Token variadic_token = current_token_;
      AdvanceToken();  // skip ...

      Token expected_type_token = current_token_;
      if (std::optional<ParsedType> type = ParseType()) {
        variadic_type = VariadicType{std::move(type.value()),
                                     std::move(variadic_token.meta)};
      } else {
        error_collector_.Add("variadic requires a type to be specified",
                             expected_type_token.meta);
      }

      if (current_token_.kind != TokenKind::kCloseParen) {
        error_collector_.Add("'...' must be the last argument in a function",
                             current_token_.meta);
        if (!SynchronizeOnError([](TokenKind kind) {
              return kind == TokenKind::kCloseParen;
            })) {
          return std::nullopt;
        }
      }
      break;  // variadic MUST be the last argument
    }

#define HANDLE_ERROR_IN_ARGUMENT()                                          \
  if (!SynchronizeOnError([](TokenKind kind) {                              \
        return kind == TokenKind::kComma || kind == TokenKind::kCloseParen; \
      })) {                                                                 \
    return std::nullopt;                                                    \
  }                                                                         \
  if (current_token_.kind == TokenKind::kComma)                             \
    AdvanceToken();                                                         \
  continue

    Token arg_name = current_token_;
    if (!ConsumeToken(TokenKind::kIdent, "expected argument name")) {
      HANDLE_ERROR_IN_ARGUMENT();
    }

    if (!ConsumeToken(TokenKind::kColon, "expected ':' after argument name")) {
      HANDLE_ERROR_IN_ARGUMENT();
    }

    std::optional<ParsedType> arg_type = ParseType();
    if (!arg_type.has_value()) {
      error_collector_.Add("expected valid type name for argument",
                           current_token_.meta);
      HANDLE_ERROR_IN_ARGUMENT();
    }

    arguments.emplace_back(SpannedText::FromToken(std::move(arg_name)),
                           std::move(arg_type.value()));

    if (current_token_.kind == TokenKind::kComma) {
      AdvanceToken();  // consume ','
    } else if (current_token_.kind != TokenKind::kCloseParen) {
      error_collector_.Add("expected ',' or ')' after argument",
                           current_token_.meta);

      if (!SynchronizeOnError([](TokenKind kind) {
            return kind == TokenKind::kComma || kind == TokenKind::kCloseParen;
          })) {
        return std::nullopt;
      }

      if (current_token_.kind == TokenKind::kComma) {
        AdvanceToken();
      }
    }
  }

  ConsumeToken(TokenKind::kCloseParen, "expected ')' to close argument list");
  return FunctionArgumentList{std::move(arguments), std::move(variadic_type)};
}

void Parser::AdvanceToken() {
  current_token_ = tokenizer_.next();
  while (true) {
    if (current_token_.kind == TokenKind::kTokenError) {
      error_collector_.Add(current_token_.value, current_token_.meta);
      current_token_ = tokenizer_.next();
      continue;
    }

    if (current_token_.kind == TokenKind::kUnknown) {
      error_collector_.Add("unknown token: \"" + current_token_.value + "\"",
                           current_token_.meta);
      current_token_ = tokenizer_.next();
      continue;
    }

    if (current_token_.kind == TokenKind::kComment) {
      current_token_ = tokenizer_.next();
      continue;
    }

    break;
  }
}

bool Parser::ConsumeToken(TokenKind expected_kind,
                          std::string_view error_message) {
  if (current_token_.kind != expected_kind) {
    error_collector_.Add(error_message, current_token_.meta);
    return false;
  }

  AdvanceToken();
  return true;
}

bool Parser::SynchronizeOnError(std::function<bool(TokenKind)> is_target) {
  // A token may simply have been missing in which case we may already be sync'd
  if (is_target(current_token_.kind))
    return true;

  AdvanceToken();  // Always advance to prevent infinite loops

  auto is_start_of_statement = [](TokenKind kind) {
    return kind == TokenKind::kKwLet || kind == TokenKind::kKwStruct ||
           kind == TokenKind::kKwExtern || kind == TokenKind::kKwWhile ||
           kind == TokenKind::kKwReturn || kind == TokenKind::kKwBreak ||
           kind == TokenKind::kKwFn || kind == TokenKind::kKwContinue ||
           kind == TokenKind::kKwThrow || kind == TokenKind::kKwImport ||
           kind == TokenKind::kKwAlias;
  };

  while (current_token_.kind != TokenKind::kEndOfFile) {
    if (is_target(current_token_.kind))
      return true;

    if (is_start_of_statement(current_token_.kind))
      return false;

    AdvanceToken();
  }

  return false;
}
