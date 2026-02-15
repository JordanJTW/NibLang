#include "compiler/parser.h"

#include <cstddef>
#include <iomanip>
#include <string>
#include <string_view>

#include "compiler/assembler.h"
#include "compiler/tokenizer.h"
#include "compiler/types.h"

template <typename T>
std::unique_ptr<Expression> make_primary_expression(T&& value) {
  return std::make_unique<Expression>(
      Expression{PrimaryExpression{std::forward<T>(value)}});
}

std::unique_ptr<Expression> make_binary_expression(
    TokenKind op,
    std::unique_ptr<Expression> lhs,
    std::unique_ptr<Expression> rhs) {
  return std::make_unique<Expression>(
      Expression{BinaryExpression{op, std::move(lhs), std::move(rhs)}});
}

void print_error(const std::string& file,
                 Token token,
                 std::string_view message) {
  size_t line_start = file.rfind('\n', token.idx);
  if (line_start == std::string_view::npos)
    line_start = 0;
  else
    ++line_start;

  size_t line_end = file.find('\n', token.idx);
  if (line_end == std::string_view::npos)
    line_end = file.size();

  std::string line = file.substr(line_start, line_end - line_start);
  size_t relative_offset = token.idx - line_start;

  std::string kErrorPrefix = "Error: " + std::to_string(token.meta.line) + ": ";
  std::cerr << kErrorPrefix << line << std::endl
            << std::setw(kErrorPrefix.length() + relative_offset) << " "
            << "^ " << message << std::endl;
}

Block Parser::Parse() {
  Block root_block;

  Token token = tokenizer_.next();
  ParseBlock(token, root_block);

  return root_block;
}

void Parser::ParseBlock(Token& token, Block& block) {
  while (token.kind != TokenKind::kEndOfFile &&
         token.kind != TokenKind::kCloseBrace) {
    if (token.kind == TokenKind::kUnknown) {
      HandleError(token, "unknown token");
      token = tokenizer_.next();
      continue;
    }

    if (token.kind == TokenKind::kComment) {
      token = tokenizer_.next();
      continue;
    }

    Token start = token;
    // Function definition i.e. fn $name($args,...):
    if (token.kind == TokenKind::kKwFn) {
      token = tokenizer_.next();

      const Token name = token;
      if (name.kind != TokenKind::kIdent) {
        HandleError(token, "requires a function name");
        token = tokenizer_.next();
        continue;
      }

      token = tokenizer_.next();
      if (token.kind != TokenKind::kOpenParen) {
        HandleError(token, "expected (");
        token = tokenizer_.next();
        continue;
      }
      token = tokenizer_.next();  // after '('

      std::vector<std::pair<std::string, std::string>> arguments;
      if (token.kind != TokenKind::kCloseParen) {
        while (true) {
          if (token.kind != TokenKind::kIdent) {
            HandleError(token, "expected parameter name");
            break;
          }

          std::string param_name = token.value;
          token = tokenizer_.next();  // after parameter name

          if (token.kind != TokenKind::kColon) {
            HandleError(token, "expected ':' after parameter name");
            break;
          }

          token = tokenizer_.next();  // after ':'

          if (token.kind != TokenKind::kIdent) {
            HandleError(token, "expected type name");
            break;
          }

          std::string param_type = token.value;
          arguments.emplace_back(param_name, param_type);
          token = tokenizer_.next();  // after type

          if (token.kind == TokenKind::kComma) {
            token = tokenizer_.next();
            continue;  // next parameter
          }

          if (token.kind == TokenKind::kCloseParen)
            break;  // done

          HandleError(token, "expected ',' or ')'");
          break;
        }
      }

      token = tokenizer_.next();  // after ')'

      std::vector<std::string> return_types;
      if (token.kind == TokenKind::kSkinnyArrow) {
        token = tokenizer_.next();  // consume '->'

        if (token.kind != TokenKind::kIdent) {
          HandleError(token, "expected return type");
        } else {
          return_types.push_back(token.value);
          token = tokenizer_.next();

          while (token.kind == TokenKind::kPipe) {
            token = tokenizer_.next();  // consume '|'

            if (token.kind != TokenKind::kIdent) {
              HandleError(token, "expected type after '|'");
              break;
            }

            return_types.push_back(token.value);
            token = tokenizer_.next();
          }
        }
      }

      FunctionDeclaration fn{name.value, std::move(arguments),
                             std::move(return_types), Block{}};

      // ---- now expect '{' ----
      if (token.kind != TokenKind::kOpenBrace) {
        HandleError(token, "expected '{'");
        continue;
      }

      token = tokenizer_.next();  // afer '{'
      ParseBlock(token, fn.body);

      block.statements.push_back(
          std::make_unique<Statement>(Statement{std::move(fn)}));
      continue;
    }

    if (token.kind == TokenKind::kKwWhile) {
      MetaInfo meta = token.meta;
      token = tokenizer_.next();

      if (token.kind != TokenKind::kOpenParen) {
        HandleError(token, "expected (");
        continue;
      }
      token = tokenizer_.next();  // after '('

      auto condition = ParseExpression(token);
      if (!condition) {
        HandleError(token, "invalid expression");
        continue;
      }

      if (token.kind != TokenKind::kCloseParen) {
        HandleError(token, "expected ')'");
        continue;
      }

      token = tokenizer_.next();  // after ')'

      WhileStatement while_stmt{meta.line, std::move(condition), Block{}};

      if (token.kind != TokenKind::kOpenBrace) {
        HandleError(token, "expected '{'");
        continue;
      }

      token = tokenizer_.next();
      ParseBlock(token, while_stmt.body);

      block.statements.push_back(
          std::make_unique<Statement>(Statement{std::move(while_stmt)}));
      continue;
    }

    if (token.kind == TokenKind::kKwIf) {
      MetaInfo meta = token.meta;
      token = tokenizer_.next();

      if (token.kind != TokenKind::kOpenParen) {
        HandleError(token, "expected (");
        continue;
      }
      token = tokenizer_.next();  // after '('

      auto condition = ParseExpression(token);
      if (!condition) {
        HandleError(token, "invalid expression");
        continue;
      }

      if (token.kind != TokenKind::kCloseParen) {
        HandleError(token, "expected ')'");
        continue;
      }

      token = tokenizer_.next();  // after ')'

      IfStatement if_stmt{meta.line, std::move(condition), Block{}, Block{}};

      if (token.kind != TokenKind::kOpenBrace) {
        HandleError(token, "expected '{'");
        continue;
      }

      token = tokenizer_.next();
      ParseBlock(token, if_stmt.then_body);

      if (token.kind == TokenKind::kKwElse) {
        token = tokenizer_.next();
        if (token.kind != TokenKind::kOpenBrace) {
          HandleError(token, "expected '{'");
          continue;
        }

        token = tokenizer_.next();
        ParseBlock(token, if_stmt.else_body);
      }

      block.statements.push_back(
          std::make_unique<Statement>(Statement{std::move(if_stmt)}));
      continue;
    }

    if (token.kind == TokenKind::kKwReturn) {
      token = tokenizer_.next();
      if (auto expr = ParseExpression(token)) {
        if (token.kind != TokenKind::kEndExpr) {
          HandleError(token, "expected ;");
          continue;
        } else {
          token = tokenizer_.next();  // consume ;
        }
        block.statements.push_back(std::make_unique<Statement>(
            Statement{ReturnStatement{std::move(expr)}}));
      }
      continue;
    }

    if (token.kind == TokenKind::kKwThrow) {
      token = tokenizer_.next();
      if (auto expr = ParseExpression(token)) {
        if (token.kind != TokenKind::kEndExpr) {
          HandleError(token, "expected ;");
          continue;
        } else {
          token = tokenizer_.next();  // consume ;
        }
        block.statements.push_back(std::make_unique<Statement>(
            Statement{ThrowStatement{std::move(expr)}}));
      }
      continue;
    }

    if (token.kind == TokenKind::kKwBreak) {
      token = tokenizer_.next();
      if (token.kind != TokenKind::kEndExpr) {
        HandleError(token, "expected ;");
        continue;
      } else {
        token = tokenizer_.next();  // consume ;
      }
      block.statements.push_back(
          std::make_unique<Statement>(Statement{BreakStatement{}}));
      continue;
    }

    if (token.kind == TokenKind::kKwContinue) {
      token = tokenizer_.next();
      if (token.kind != TokenKind::kEndExpr) {
        HandleError(token, "expected ;");
        continue;
      } else {
        token = tokenizer_.next();  // consume ;
      }
      block.statements.push_back(
          std::make_unique<Statement>(Statement{ContinueStatement{}}));
      continue;
    }

    if (auto expr = ParseExpression(token)) {
      if (token.kind != TokenKind::kEndExpr) {
        HandleError(token, "expected ;");
        continue;
      } else {
        block.statements.push_back(
            std::make_unique<Statement>(Statement{std::move(expr)}));
        token = tokenizer_.next();  // consume ;
      }
      continue;
    }

    HandleError(token, "unexpected token");
    token = tokenizer_.next();
  }

  token = tokenizer_.next();  // consume '}' or EOF
}

std::unique_ptr<Expression> Parser::ParseValue(const Token& value) {
  switch (value.kind) {
    case TokenKind::kNumber: {
      if ((value.value.find('.') != std::string::npos) ||
          value.value.back() == 'f') {
        float f32 = std::stof(value.value);
        return make_primary_expression(f32);
      } else {
        int i32 = std::stoi(value.value);
        return make_primary_expression(i32);
      }
    }
    case TokenKind::kIdent: {
      return make_primary_expression(Identifier{.name = value.value});
    }
    case TokenKind::kString: {
      return make_primary_expression(StringLiteral{value.value});
    }
    case TokenKind::kKwTrue: {
      return make_primary_expression(true);
    }
    case TokenKind::kKwFalse: {
      return make_primary_expression(false);
    }
    default:
      print_error(text_, value, "unassignable type");
      return nullptr;
  }
}

std::unique_ptr<Expression> Parser::ParseExpression(Token& token) {
  return ParseAssignment(token);
}

std::unique_ptr<Expression> Parser::ParseAssignment(Token& token) {
  Token entry_token = token;
  if (token.kind == TokenKind::kIdent) {
    token = tokenizer_.next();
    if (token.kind == TokenKind::kAssign) {
      token = tokenizer_.next();  // consume '='

      auto rhs = ParseExpression(token);
      if (!rhs)
        return nullptr;

      return std::make_unique<Expression>(
          Expression{AssignmentExpression{entry_token.value, std::move(rhs)}});
    }
  }

  token = tokenizer_.seekTo(entry_token);
  return ParseComparison(token);
}

std::unique_ptr<Expression> Parser::ParseComparison(Token& token) {
  auto lhs = ParseAdditive(token);
  if (!lhs)
    return nullptr;

  while (token.kind == TokenKind::kCompareEq ||
         token.kind == TokenKind::kCompareNe ||
         token.kind == TokenKind::kCompareLt ||
         token.kind == TokenKind::kCompareLe ||
         token.kind == TokenKind::kCompareGt ||
         token.kind == TokenKind::kCompareGe) {
    Token op = token;
    token = tokenizer_.next();

    auto rhs = ParseAdditive(token);
    if (!rhs)
      return nullptr;

    lhs = make_binary_expression(op.kind, std::move(lhs), std::move(rhs));
  }
  return lhs;
}

std::unique_ptr<Expression> Parser::ParseAdditive(Token& token) {
  auto lhs = ParseMultiplicative(token);
  if (!lhs)
    return nullptr;

  while (token.kind == TokenKind::kAdd || token.kind == TokenKind::kSubtract) {
    Token op = token;
    token = tokenizer_.next();

    auto rhs = ParseMultiplicative(token);
    if (!rhs)
      return nullptr;

    lhs = make_binary_expression(op.kind, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

std::unique_ptr<Expression> Parser::ParseMultiplicative(Token& token) {
  auto lhs = ParsePrimary(token);
  if (!lhs)
    return nullptr;

  while (token.kind == TokenKind::kMultiply ||
         token.kind == TokenKind::kDivide) {
    Token op = token;
    token = tokenizer_.next();

    auto rhs = ParsePrimary(token);
    if (!rhs)
      return nullptr;

    lhs = make_binary_expression(op.kind, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

std::unique_ptr<Expression> Parser::ParsePrimary(Token& token) {
  switch (token.kind) {
    case TokenKind::kNumber:
    case TokenKind::kString:
    case TokenKind::kIdent:
    case TokenKind::kKwTrue:
    case TokenKind::kKwFalse: {
      Token value = token;
      token = tokenizer_.next();

      // Function call?
      if (value.kind == TokenKind::kIdent &&
          token.kind == TokenKind::kOpenParen) {
        return ParseCall(token, value);
      }

      return ParseValue(value);
    }

    case TokenKind::kOpenParen: {
      token = tokenizer_.next();  // consume '('

      auto expr = ParseExpression(token);
      if (!expr)
        return nullptr;

      if (token.kind != TokenKind::kCloseParen) {
        HandleError(token, "expected ')'");
        return nullptr;
      }

      token = tokenizer_.next();  // consume ')'
      return expr;
    }

    default:
      HandleError(token, "expected expression");
      return nullptr;
  }
}

std::unique_ptr<Expression> Parser::ParseCall(Token& token, Token fn_name) {
  token = tokenizer_.next();  // consume '('

  std::vector<std::unique_ptr<Expression>> arguments;
  if (token.kind != TokenKind::kCloseParen) {
    while (true) {
      auto arg = ParseExpression(token);
      if (!arg)
        return nullptr;
      arguments.push_back(std::move(arg));

      if (token.kind == TokenKind::kCloseParen)
        break;

      if (token.kind != TokenKind::kComma) {
        HandleError(token, "expected ',' or ')'");
        return nullptr;
      }

      token = tokenizer_.next();
    }
  }

  token = tokenizer_.next();  // consume ')'

  return std::make_unique<Expression>(
      Expression{CallExpression{fn_name.value, std::move(arguments)}});
}

void Parser::HandleError(Token& token, const std::string& message) {
  print_error(text_, token, message);
  while (token.kind != TokenKind::kEndExpr &&
         token.kind != TokenKind::kEndOfFile)
    token = tokenizer_.next();
}
