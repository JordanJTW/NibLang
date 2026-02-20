#include "compiler/parser.h"

#include <cstddef>
#include <iomanip>
#include <string>
#include <string_view>

#include "compiler/assembler.h"
#include "compiler/logging.h"
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

TextRange make_text_range(Token start, Token end) {
  return {start.idx, end.idx};
}

Block Parser::Parse() {
  Block root_block;
  ParseBlock(root_block);
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

void Parser::ParseBlock(Block& block) {
  while (current_token_.kind != TokenKind::kEndOfFile &&
         current_token_.kind != TokenKind::kCloseBrace) {
    if (current_token_.kind == TokenKind::kUnknown) {
      HandleError("unknown token");
      current_token_ = tokenizer_.next();
      continue;
    }

    if (current_token_.kind == TokenKind::kComment) {
      current_token_ = tokenizer_.next();
      continue;
    }

    Token start = current_token_;
    // Function definition i.e. fn $name($args,...):
    if (current_token_.kind == TokenKind::kKwFn) {
      if (auto fn = ParseFunctionDeclaration()) {
        block.statements.push_back(std::make_unique<Statement>(
            Statement{std::move(*(fn.release())),
                      make_text_range(start, current_token_)}));
      }
      continue;
    }

    // struct definition i.e. struct $name { $field: $type, ... }
    if (current_token_.kind == TokenKind::kKwStruct ||
        current_token_.kind == TokenKind::kKwExtern) {
      if (auto struct_decl = ParseStructDeclaration()) {
        block.statements.push_back(std::make_unique<Statement>(
            Statement{std::move(*(struct_decl.release())),
                      make_text_range(start, current_token_)}));
      }
      continue;
    }

    if (current_token_.kind == TokenKind::kKwWhile) {
      MetaInfo meta = current_token_.meta;
      current_token_ = tokenizer_.next();

      CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kOpenParen,
                                        "expected '(' before while condition"));

      ASSIGN_OR_CONTINUE(condition_expr, ParseExpression());

      CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kCloseParen,
                                        "expected ')' after while condition"));

      WhileStatement while_stmt{meta.line, std::move(condition_expr), Block{}};

      CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kOpenBrace,
                                        "expected '{' before while body"));

      ParseBlock(while_stmt.body);

      block.statements.push_back(std::make_unique<Statement>(Statement{
          std::move(while_stmt), make_text_range(start, current_token_)}));
      continue;
    }

    if (current_token_.kind == TokenKind::kKwIf) {
      MetaInfo meta = current_token_.meta;
      current_token_ = tokenizer_.next();

      CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kOpenParen,
                                        "expected '(' before if condition"));

      ASSIGN_OR_CONTINUE(condition, ParseExpression());

      CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kCloseParen,
                                        "expected ')' after if condition"));

      IfStatement if_stmt{meta.line, std::move(condition), Block{}, Block{}};

      CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kOpenBrace,
                                        "expected '{' before if body"));

      ParseBlock(if_stmt.then_body);

      // Optional else block
      if (current_token_.kind == TokenKind::kKwElse) {
        current_token_ = tokenizer_.next();

        CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kOpenBrace,
                                          "expected '{' before else body"));

        ParseBlock(if_stmt.else_body);
      }

      block.statements.push_back(std::make_unique<Statement>(Statement{
          std::move(if_stmt), make_text_range(start, current_token_)}));
      continue;
    }

    if (current_token_.kind == TokenKind::kKwReturn) {
      current_token_ = tokenizer_.next();
      if (auto expr = ParseExpression()) {
        CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kEndExpr,
                                          "expected ';' after return value"));
        block.statements.push_back(std::make_unique<Statement>(
            Statement{ReturnStatement{std::move(expr)},
                      make_text_range(start, current_token_)}));
      }
      continue;
    }

    if (current_token_.kind == TokenKind::kKwThrow) {
      current_token_ = tokenizer_.next();
      if (auto expr = ParseExpression()) {
        CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kEndExpr,
                                          "expected ';' after throw value"));
        block.statements.push_back(std::make_unique<Statement>(
            Statement{ThrowStatement{std::move(expr)},
                      make_text_range(start, current_token_)}));
      }
      continue;
    }

    if (current_token_.kind == TokenKind::kKwBreak) {
      current_token_ = tokenizer_.next();
      CONTINUE_IF_FALSE(
          ExpectNextToken(TokenKind::kEndExpr, "expected ';' after break"));
      block.statements.push_back(std::make_unique<Statement>(
          Statement{BreakStatement{}, make_text_range(start, current_token_)}));
      continue;
    }

    if (current_token_.kind == TokenKind::kKwContinue) {
      current_token_ = tokenizer_.next();  // consume 'continue'

      CONTINUE_IF_FALSE(
          ExpectNextToken(TokenKind::kEndExpr, "expected ';' after continue"));

      block.statements.push_back(std::make_unique<Statement>(Statement{
          ContinueStatement{}, make_text_range(start, current_token_)}));
      continue;
    }

    if (current_token_.kind == TokenKind::kKwLet) {
      current_token_ = tokenizer_.next();  // consume `let`

      ASSIGN_OR_CONTINUE(
          variable_name,
          ExpectNextToken(TokenKind::kIdent, "expected variable name"));

      std::vector<std::string> type;
      // : <type> is optional, but if there is a ':' there must be a type.
      if (current_token_.kind == TokenKind::kColon) {
        current_token_ = tokenizer_.next();  // after ':'

        type = ParseUnionTypeList();
        if (type.empty()) {
          HandleError("expected type name");
          continue;
        }
      }

      CONTINUE_IF_FALSE(
          ExpectNextToken(TokenKind::kAssign, "expected assign (=) operator"));

      ASSIGN_OR_CONTINUE(value_expr, ParseExpression());

      CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kEndExpr, "expected ';'"));

      std::string assignment_type;
      if (!type.empty())
        assignment_type = type[0];
      block.statements.push_back(
          std::make_unique<Statement>(Statement{AssignStatement{
              variable_name->value, assignment_type, std::move(value_expr)}}));
      continue;
    }

    if (auto expr = ParseExpression()) {
      if (current_token_.kind != TokenKind::kEndExpr) {
        HandleError("expected ;");
        continue;
      } else {
        block.statements.push_back(std::make_unique<Statement>(Statement{
            std::move(expr), make_text_range(start, current_token_)}));
        current_token_ = tokenizer_.next();  // consume ;
      }
      continue;
    }

    HandleError("unexpected token");
    current_token_ = tokenizer_.next();
  }

  current_token_ = tokenizer_.next();  // consume '}' or EOF
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

std::unique_ptr<Expression> Parser::ParseExpression() {
  return ParseAssignment();
}

std::unique_ptr<Expression> Parser::ParseAssignment() {
  Token entry_token = current_token_;

  auto expr = ParsePostFix();
  if (!expr)
    return nullptr;

  if (current_token_.kind == TokenKind::kAssign) {
    current_token_ = tokenizer_.next();  // consume '='

    auto rhs = ParseExpression();
    if (!rhs)
      return nullptr;

    return std::make_unique<Expression>(
        Expression{AssignmentExpression{std::move(expr), std::move(rhs)}});
  }

  current_token_ = tokenizer_.seekTo(entry_token);
  return ParseLogical();
}

std::unique_ptr<Expression> Parser::ParseLogical() {
  auto lhs = ParseComparison();
  if (!lhs)
    return nullptr;

  while (current_token_.kind == TokenKind::kAndAnd ||
         current_token_.kind == TokenKind::kOrOr) {
    Token op = current_token_;
    current_token_ = tokenizer_.next();

    auto rhs = ParseComparison();
    if (!rhs)
      return nullptr;

    lhs = std::make_unique<Expression>(Expression{
        LogicExpression{op.kind == TokenKind::kAndAnd ? LogicExpression::AND
                                                      : LogicExpression::OR,
                        std::move(lhs), std::move(rhs)}});
  }
  return lhs;
}

std::unique_ptr<Expression> Parser::ParseComparison() {
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
    current_token_ = tokenizer_.next();

    auto rhs = ParseAdditive();
    if (!rhs)
      return nullptr;

    lhs = make_binary_expression(op.kind, std::move(lhs), std::move(rhs));
  }
  return lhs;
}

std::unique_ptr<Expression> Parser::ParseAdditive() {
  auto lhs = ParseMultiplicative();
  if (!lhs)
    return nullptr;

  while (current_token_.kind == TokenKind::kAdd ||
         current_token_.kind == TokenKind::kSubtract) {
    Token op = current_token_;
    current_token_ = tokenizer_.next();

    auto rhs = ParseMultiplicative();
    if (!rhs)
      return nullptr;

    lhs = make_binary_expression(op.kind, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

std::unique_ptr<Expression> Parser::ParseMultiplicative() {
  auto lhs = ParsePostFix();
  if (!lhs)
    return nullptr;

  while (current_token_.kind == TokenKind::kMultiply ||
         current_token_.kind == TokenKind::kDivide) {
    Token op = current_token_;
    current_token_ = tokenizer_.next();

    auto rhs = ParsePostFix();
    if (!rhs)
      return nullptr;

    lhs = make_binary_expression(op.kind, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

std::unique_ptr<Expression> Parser::ParsePostFix() {
  auto expr = ParsePrimary();
  if (!expr)
    return nullptr;

  while (true) {
    // Handle function call i.e. $expr(...)
    if (current_token_.kind == TokenKind::kOpenParen) {
      expr = ParseCall(std::move(expr));
      if (!expr)
        return nullptr;
      continue;
    }

    if (current_token_.kind == TokenKind::kDot) {
      current_token_ = tokenizer_.next();  // consume '.'

      if (current_token_.kind != TokenKind::kIdent) {
        HandleError("expected identifier after '.'");
        return nullptr;
      }
      Token ident = current_token_;
      current_token_ = tokenizer_.next();  // consume identifier

      expr = std::make_unique<Expression>(
          Expression{MemberAccessExpression{std::move(expr), ident.value}});
      continue;
    }

    if (current_token_.kind == TokenKind::kSquareOpen) {
      current_token_ = tokenizer_.next();  // consume '['

      auto index = ParseExpression();
      if (!index)
        return nullptr;

      if (current_token_.kind != TokenKind::kSquareClose) {
        HandleError("expected ']'");
        return nullptr;
      }

      current_token_ = tokenizer_.next();  // consume ']'

      expr = std::make_unique<Expression>(
          Expression{ArrayAccessExpression{std::move(expr), std::move(index)}});
      continue;
    }
    break;
  }

  return expr;
}

std::unique_ptr<Expression> Parser::ParsePrimary() {
  switch (current_token_.kind) {
    case TokenKind::kNumber:
    case TokenKind::kString:
    case TokenKind::kIdent:
    case TokenKind::kKwTrue:
    case TokenKind::kKwFalse: {
      Token value = current_token_;
      current_token_ = tokenizer_.next();
      return ParseValue(value);
    }

    case TokenKind::kOpenParen: {
      current_token_ = tokenizer_.next();  // consume '('

      auto expr = ParseExpression();
      if (!expr)
        return nullptr;

      if (current_token_.kind != TokenKind::kCloseParen) {
        HandleError("expected ')'");
        return nullptr;
      }

      current_token_ = tokenizer_.next();  // consume ')'
      return expr;
    }

    default:
      HandleError("expected expression");
      return nullptr;
  }
}

std::unique_ptr<Expression> Parser::ParseCall(
    std::unique_ptr<Expression> callee) {
  current_token_ = tokenizer_.next();  // consume '('

  std::vector<std::unique_ptr<Expression>> arguments;
  if (current_token_.kind != TokenKind::kCloseParen) {
    while (true) {
      auto arg = ParseExpression();
      if (!arg)
        return nullptr;
      arguments.push_back(std::move(arg));

      if (current_token_.kind == TokenKind::kCloseParen)
        break;

      if (current_token_.kind != TokenKind::kComma) {
        HandleError("expected ',' or ')'");
        return nullptr;
      }

      current_token_ = tokenizer_.next();
    }
  }

  current_token_ = tokenizer_.next();  // consume ')'

  return std::make_unique<Expression>(
      Expression{CallExpression{std::move(callee), std::move(arguments)}});
}

std::vector<std::string> Parser::ParseUnionTypeList() {
  std::vector<std::string> return_types;
  if (current_token_.kind != TokenKind::kIdent) {
    HandleError("expected type");
  } else {
    return_types.push_back(current_token_.value);
    current_token_ = tokenizer_.next();

    while (current_token_.kind == TokenKind::kPipe) {
      current_token_ = tokenizer_.next();  // consume '|'

      if (current_token_.kind != TokenKind::kIdent) {
        HandleError("expected type after '|'");
        break;
      }

      return_types.push_back(current_token_.value);
      current_token_ = tokenizer_.next();
    }
  }
  return return_types;
}

std::unique_ptr<StructDeclaration> Parser::ParseStructDeclaration() {
  bool is_extern = current_token_.kind == TokenKind::kKwExtern;
  if (is_extern)
    current_token_ = tokenizer_.next();  // consume 'extern'

  if (current_token_.kind != TokenKind::kKwStruct) {
    HandleError("expected 'struct'");
    return nullptr;
  }
  current_token_ = tokenizer_.next();  // consume 'struct'

  if (current_token_.kind != TokenKind::kIdent) {
    HandleError("expected struct name");
    return nullptr;
  }

  Token struct_name = current_token_;
  current_token_ = tokenizer_.next();  // consume struct name

  if (current_token_.kind != TokenKind::kOpenBrace) {
    HandleError("expected '{' after struct name");
    return nullptr;
  }

  auto struct_decl = std::make_unique<StructDeclaration>();
  struct_decl->name = struct_name.value;
  struct_decl->is_extern = is_extern;

  current_token_ = tokenizer_.next();  // consume '{'
  while (current_token_.kind != TokenKind::kCloseBrace &&
         current_token_.kind != TokenKind::kEndOfFile) {
    if (current_token_.kind == TokenKind::kKwFn) {
      auto method = ParseFunctionDeclaration();
      if (!method) {
        return nullptr;
      }
      struct_decl->methods.emplace_back(method->name, std::move(*method));
      continue;
    }

    if (current_token_.kind == TokenKind::kIdent) {
      Token field_name = current_token_;
      current_token_ = tokenizer_.next();  // consume <name>

      if (current_token_.kind != TokenKind::kColon) {
        HandleError("expected ':' after field name");
        return nullptr;
      }

      current_token_ = tokenizer_.next();  // consume ':'

      std::vector<std::string> type = ParseUnionTypeList();
      if (type.empty()) {
        HandleError("expected type name");
        return nullptr;
      }

      struct_decl->fields.emplace_back(field_name.value, type[0]);

      if (current_token_.kind != TokenKind::kEndExpr) {
        HandleError("expected ';'");
        return nullptr;
      }
      current_token_ = tokenizer_.next();
      continue;
    }

    if (current_token_.kind == TokenKind::kComment) {
      current_token_ = tokenizer_.next();
      continue;
    }

    if (current_token_.kind == TokenKind::kCloseBrace)
      break;

    HandleError("invalid struct member declaration");
    return nullptr;
  }
  current_token_ = tokenizer_.next();  // consume '}'
  return struct_decl;
}

std::unique_ptr<FunctionDeclaration> Parser::ParseFunctionDeclaration() {
  current_token_ = tokenizer_.next();

  const Token name = current_token_;
  if (name.kind != TokenKind::kIdent) {
    HandleError("requires a function name");
    return nullptr;
  }
  current_token_ = tokenizer_.next();  // after function name

  if (current_token_.kind != TokenKind::kOpenParen) {
    HandleError("expected (");
    return nullptr;
  }
  current_token_ = tokenizer_.next();  // after '('

  std::vector<std::pair<std::string, std::string>> arguments;
  if (current_token_.kind != TokenKind::kCloseParen) {
    while (true) {
      if (current_token_.kind != TokenKind::kIdent) {
        HandleError("expected parameter name");
        break;
      }

      std::string param_name = current_token_.value;
      current_token_ = tokenizer_.next();  // after parameter name

      if (current_token_.kind != TokenKind::kColon) {
        HandleError("expected ':' after parameter name");
        break;
      }

      current_token_ = tokenizer_.next();  // after ':'

      if (current_token_.kind != TokenKind::kIdent) {
        HandleError("expected type name");
        break;
      }

      std::string param_type = current_token_.value;
      arguments.emplace_back(param_name, param_type);
      current_token_ = tokenizer_.next();  // after type

      if (current_token_.kind == TokenKind::kComma) {
        current_token_ = tokenizer_.next();
        continue;  // next parameter
      }

      if (current_token_.kind == TokenKind::kCloseParen)
        break;  // done

      HandleError("expected ',' or ')'");
      break;
    }
  }
  current_token_ = tokenizer_.next();  // after ')'

  std::vector<std::string> return_types;
  if (current_token_.kind == TokenKind::kSkinnyArrow) {
    current_token_ = tokenizer_.next();  // consume '->'

    return_types = ParseUnionTypeList();
  }

  FunctionDeclaration fn{name.value, std::move(arguments),
                         std::move(return_types)};

  if (current_token_.kind == TokenKind::kEndExpr) {
    current_token_ = tokenizer_.next();  // consume ';'
    return std::make_unique<FunctionDeclaration>(std::move(fn));
  }

  if (!ExpectNextToken(TokenKind::kOpenBrace, "expected function body"))
    return nullptr;

  Block body;
  ParseBlock(body);
  fn.body = std::make_unique<Block>(std::move(body));
  return std::make_unique<FunctionDeclaration>(std::move(fn));
}

std::optional<Token> Parser::ExpectNextToken(TokenKind expected_kind,
                                             std::string_view error_message) {
  if (current_token_.kind != expected_kind) {
    HandleError(error_message);
    return std::nullopt;
  }

  Token expected_token = current_token_;
  current_token_ = tokenizer_.next();
  return expected_token;
}

void Parser::HandleError(std::string_view message) {
  print_error(text_, current_token_, message);

  auto is_start_of_statement = [](TokenKind kind) {
    return kind == TokenKind::kKwLet || kind == TokenKind::kKwStruct ||
           kind == TokenKind::kKwExtern || kind == TokenKind::kKwWhile ||
           kind == TokenKind::kKwReturn || kind == TokenKind::kKwBreak ||
           kind == TokenKind::kKwFn || kind == TokenKind::kKwContinue ||
           kind == TokenKind::kKwThrow;
  };

  while (!is_start_of_statement(current_token_.kind) &&
         current_token_.kind != TokenKind::kEndExpr &&
         current_token_.kind != TokenKind::kEndOfFile)
    current_token_ = tokenizer_.next();

  // Sync past ';' as that does not start a statement
  if (current_token_.kind == TokenKind::kEndExpr)
    current_token_ = tokenizer_.next();
}
