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
std::unique_ptr<Expression> make_primary_expression(T&& value,
                                                    const Token& token) {
  return std::make_unique<Expression>(
      Expression{PrimaryExpression{std::forward<T>(value)}, token.meta});
}

void print_error(const std::string& file,
                 Metadata metadata,
                 std::string_view message) {
  size_t line_start = file.rfind('\n', metadata.column_range.start);
  if (line_start == std::string_view::npos)
    line_start = 0;
  else
    ++line_start;

  size_t line_end = file.find('\n', metadata.column_range.start);
  if (line_end == std::string_view::npos)
    line_end = file.size();

  std::string line = file.substr(line_start, line_end - line_start);
  size_t relative_offset = metadata.column_range.start - line_start;

  std::string kErrorPrefix =
      "Error: " + std::to_string(metadata.line_range.start) + ": ";
  std::cerr << kErrorPrefix << line << std::endl
            << std::setw(kErrorPrefix.length() + relative_offset) << " "
            << "^ " << message << std::endl;
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
      AdvanceToken();
      continue;
    }

    Token start_token = current_token_;

    bool is_extern = false;
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
      HandleError("extern is only valid before struct/fn");
      continue;
    }

    if (current_token_.kind == TokenKind::kKwWhile) {
      AdvanceToken();

      CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kOpenParen,
                                        "expected '(' before while condition"));

      ASSIGN_OR_CONTINUE(condition_expr, ParseExpression());

      CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kCloseParen,
                                        "expected ')' after while condition"));

      WhileStatement while_stmt{std::move(condition_expr), Block{}};

      CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kOpenBrace,
                                        "expected '{' before while body"));

      ParseBlock(while_stmt.body);

      block.statements.push_back(std::make_unique<Statement>(
          Statement{std::move(while_stmt),
                    Metadata::fromTokens(start_token, current_token_)}));
      continue;
    }

    if (current_token_.kind == TokenKind::kKwIf) {
      AdvanceToken();

      CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kOpenParen,
                                        "expected '(' before if condition"));

      ASSIGN_OR_CONTINUE(condition, ParseExpression());

      CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kCloseParen,
                                        "expected ')' after if condition"));

      IfStatement if_stmt{std::move(condition), Block{}, Block{}};

      CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kOpenBrace,
                                        "expected '{' before if body"));

      ParseBlock(if_stmt.then_body);

      // Optional else block
      if (current_token_.kind == TokenKind::kKwElse) {
        AdvanceToken();

        CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kOpenBrace,
                                          "expected '{' before else body"));

        ParseBlock(if_stmt.else_body);
      }

      block.statements.push_back(std::make_unique<Statement>(
          Statement{std::move(if_stmt),
                    Metadata::fromTokens(start_token, current_token_)}));
      continue;
    }

    if (current_token_.kind == TokenKind::kKwReturn) {
      AdvanceToken();
      if (auto expr = ParseExpression()) {
        CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kEndExpr,
                                          "expected ';' after return value"));
        block.statements.push_back(std::make_unique<Statement>(
            Statement{ReturnStatement{std::move(expr)},
                      Metadata::fromTokens(start_token, current_token_)}));
      }
      continue;
    }

    if (current_token_.kind == TokenKind::kKwThrow) {
      AdvanceToken();
      if (auto expr = ParseExpression()) {
        CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kEndExpr,
                                          "expected ';' after throw value"));
        block.statements.push_back(std::make_unique<Statement>(
            Statement{ThrowStatement{std::move(expr)},
                      Metadata::fromTokens(start_token, current_token_)}));
      }
      continue;
    }

    if (current_token_.kind == TokenKind::kKwBreak) {
      AdvanceToken();
      CONTINUE_IF_FALSE(
          ExpectNextToken(TokenKind::kEndExpr, "expected ';' after break"));
      block.statements.push_back(std::make_unique<Statement>(
          Statement{BreakStatement{},
                    Metadata::fromTokens(start_token, current_token_)}));
      continue;
    }

    if (current_token_.kind == TokenKind::kKwContinue) {
      AdvanceToken();  // consume 'continue'

      CONTINUE_IF_FALSE(
          ExpectNextToken(TokenKind::kEndExpr, "expected ';' after continue"));

      block.statements.push_back(std::make_unique<Statement>(
          Statement{ContinueStatement{},
                    Metadata::fromTokens(start_token, current_token_)}));
      continue;
    }

    if (current_token_.kind == TokenKind::kKwLet) {
      AdvanceToken();  // consume `let`

      ASSIGN_OR_CONTINUE(
          variable_name,
          ExpectNextToken(TokenKind::kIdent, "expected variable name"));

      std::optional<ParsedType> var_type;
      // : <type> is optional, but if there is a ':' there must be a type.
      if (current_token_.kind == TokenKind::kColon) {
        AdvanceToken();  // after ':'

        var_type = ParseType();
        if (!var_type.has_value()) {
          HandleError("expected type nam after ':'");
          continue;
        }
      }

      CONTINUE_IF_FALSE(
          ExpectNextToken(TokenKind::kAssign, "expected assign (=) operator"));

      ASSIGN_OR_CONTINUE(value_expr, ParseExpression());

      CONTINUE_IF_FALSE(ExpectNextToken(TokenKind::kEndExpr, "expected ';'"));

      block.statements.push_back(std::make_unique<Statement>(
          Statement{AssignStatement{variable_name->value, std::move(var_type),
                                    std::move(value_expr)},
                    Metadata::fromTokens(start_token, current_token_)}));
      continue;
    }

    if (auto expr = ParseExpression()) {
      if (current_token_.kind != TokenKind::kEndExpr) {
        HandleError("expected ;");
        continue;
      } else {
        block.statements.push_back(std::make_unique<Statement>(
            Statement{std::move(expr),
                      Metadata::fromTokens(start_token, current_token_)}));
        AdvanceToken();  // consume ;
      }
      continue;
    }

    HandleError("unexpected token");
    AdvanceToken();
  }

  AdvanceToken();  // consume '}' or EOF
}

std::unique_ptr<Expression> Parser::ParseValue(const Token& value) {
  switch (value.kind) {
    case TokenKind::kNumber: {
      if ((value.value.find('.') != std::string::npos) ||
          value.value.back() == 'f') {
        float f32 = std::stof(value.value);
        return make_primary_expression(f32, value);
      } else {
        int i32 = std::stoi(value.value);
        return make_primary_expression(i32, value);
      }
    }
    case TokenKind::kIdent: {
      return make_primary_expression(Identifier{.name = value.value}, value);
    }
    case TokenKind::kString: {
      return make_primary_expression(StringLiteral{value.value}, value);
    }
    case TokenKind::kKwTrue: {
      return make_primary_expression(true, value);
    }
    case TokenKind::kKwFalse: {
      return make_primary_expression(false, value);
    }
    default:
      print_error(text_, value.meta, "unknown literal type");
      return nullptr;
  }
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

  return ParsePostFix();
}

std::unique_ptr<Expression> Parser::ParsePostFix() {
  Token start_token = current_token_;

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

    if (current_token_.kind == TokenKind::kPlusPlus ||
        current_token_.kind == TokenKind::kMinusMinus) {
      Token op = current_token_;
      AdvanceToken();  // consume operator
      expr = std::make_unique<Expression>(
          Expression{PostfixUnaryExpression{op.kind, std::move(expr)},
                     Metadata::fromTokens(start_token, current_token_)});
      continue;
    }

    if (current_token_.kind == TokenKind::kKwAs) {
      AdvanceToken();  // consume 'as'

      std::optional<ParsedType> type = ParseType();
      if (!type)
        return nullptr;

      expr = std::make_unique<Expression>(Expression{
          TypeCastExpression{std::move(expr), std::move(type.value())},
          Metadata::fromTokens(start_token, current_token_)});
      continue;
    }

    if (current_token_.kind == TokenKind::kDot) {
      AdvanceToken();  // consume '.'

      if (current_token_.kind != TokenKind::kIdent) {
        HandleError("expected identifier after '.'");
        return nullptr;
      }
      Token ident = current_token_;
      AdvanceToken();  // consume identifier

      expr = std::make_unique<Expression>(
          Expression{MemberAccessExpression{std::move(expr), ident.value},
                     Metadata::fromTokens(start_token, current_token_)});
      continue;
    }

    if (current_token_.kind == TokenKind::kSquareOpen) {
      AdvanceToken();  // consume '['

      auto index = ParseExpression();
      if (!index)
        return nullptr;

      if (current_token_.kind != TokenKind::kSquareClose) {
        HandleError("expected ']'");
        return nullptr;
      }

      AdvanceToken();  // consume ']'

      expr = std::make_unique<Expression>(
          Expression{ArrayAccessExpression{std::move(expr), std::move(index)},
                     Metadata::fromTokens(start_token, current_token_)});
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
      AdvanceToken();
      return ParseValue(value);
    }

    case TokenKind::kOpenParen: {
      AdvanceToken();  // consume '('

      auto expr = ParseExpression();
      if (!expr)
        return nullptr;

      if (current_token_.kind != TokenKind::kCloseParen) {
        HandleError("expected ')'");
        return nullptr;
      }

      AdvanceToken();  // consume ')'
      return expr;
    }

    case TokenKind::kKwFn: {
      if (auto fn = ParseFunctionDeclaration(FunctionKind::Anonymous)) {
        return std::make_unique<Expression>(
            Expression{ClosureExpression{std::move(*fn)}});
      }
      return nullptr;
    }

    default:
      HandleError("expected expression");
      return nullptr;
  }
}

std::unique_ptr<Expression> Parser::ParseCall(
    std::unique_ptr<Expression> callee) {
  Token start_token = current_token_;
  AdvanceToken();  // consume '('

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

      AdvanceToken();
    }
  }

  AdvanceToken();  // consume ')'

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
  AdvanceToken();  // consume 'fn'

  ExpectNextToken(TokenKind::kOpenParen, "expected '('");

  std::vector<ParsedType> arg_types;
  if (current_token_.kind != TokenKind::kCloseParen) {
    while (true) {
      auto arg_type = ParseType();
      if (!arg_type)
        return std::nullopt;

      arg_types.push_back(std::move(arg_type.value()));

      if (current_token_.kind != TokenKind::kComma)
        break;

      AdvanceToken();  // consume ','
    }
  }

  ExpectNextToken(TokenKind::kCloseParen, "expected ')'");

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
  if (current_token_.kind == TokenKind::kKwFn) {
    return ParseFunctionType();
  }

  if (current_token_.kind == TokenKind::kIdent) {
    Token type_token = current_token_;
    AdvanceToken();
    return ParsedType{type_token.value, type_token.meta};
  }

  HandleError("expected type");
  return std::nullopt;
}

std::optional<StructDeclaration> Parser::ParseStructDeclaration(
    ExternStruct is_extern) {
  ExpectNextToken(TokenKind::kKwStruct, "expected 'struct'");

  if (current_token_.kind != TokenKind::kIdent) {
    HandleError("expected struct name");
    return std::nullopt;
  }

  Token struct_name = current_token_;
  AdvanceToken();  // consume struct name

  if (current_token_.kind != TokenKind::kOpenBrace) {
    HandleError("expected '{' after struct name");
    return std::nullopt;
  }

  StructDeclaration struct_decl;
  struct_decl.name = struct_name.value;
  struct_decl.is_extern = is_extern == ExternStruct::YES;

  AdvanceToken();  // consume '{'
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
        return std::nullopt;
      }
      struct_decl.methods.emplace_back(method->name, std::move(*method));
      continue;
    }

    if (static_modifier_token) {
      print_error(text_, static_modifier_token->meta,
                  "'static' is only valid on method declarations");
      // Purposefully do not attempt error recovery as we are already on a new
      // token at this point and it is only a modifier for other keywords.
      continue;
    }

    if (current_token_.kind == TokenKind::kIdent) {
      Token field_name = current_token_;
      AdvanceToken();  // consume <name>

      if (current_token_.kind != TokenKind::kColon) {
        HandleError("expected ':' after field name");
        return std::nullopt;
      }

      AdvanceToken();  // consume ':'

      std::optional<ParsedType> type = ParseType();
      if (!type.has_value()) {
        HandleError("expected type name");
        return std::nullopt;
      }

      struct_decl.fields.emplace_back(field_name.value, type.value());

      if (current_token_.kind != TokenKind::kEndExpr) {
        HandleError("expected ';'");
        return std::nullopt;
      }
      AdvanceToken();
      continue;
    }

    if (current_token_.kind == TokenKind::kCloseBrace)
      break;

    HandleError("invalid struct member declaration");
    return std::nullopt;
  }
  AdvanceToken();  // consume '}'
  return struct_decl;
}

std::optional<FunctionDeclaration> Parser::ParseFunctionDeclaration(
    FunctionKind function_kind) {
  ExpectNextToken(TokenKind::kKwFn, "expected 'fn'");

  std::string function_name;
  if (function_kind == FunctionKind::Anonymous) {
    // Perform single-token deletion recovery to try to keep parsing happy.
    if (current_token_.kind == TokenKind::kIdent) {
      print_error(text_, current_token_.meta,
                  "Anonymous functions should not have a name");
      AdvanceToken();  // skip name
    }
  } else {
    Token name = current_token_;
    if (name.kind != TokenKind::kIdent) {
      HandleError("requires a function name");
      return std::nullopt;
    }
    function_name = name.value;
    AdvanceToken();  // after function name
  }

  if (current_token_.kind != TokenKind::kOpenParen) {
    HandleError("expected (");
    return std::nullopt;
  }
  AdvanceToken();  // after '('

  bool is_variadic = false;
  std::vector<std::pair<std::string, ParsedType>> arguments;
  Token start_of_arguments_token = current_token_;
  if (current_token_.kind != TokenKind::kCloseParen) {
    while (true) {
      // extern functions support passing raw variadic arguments to the runtime.
      // This must be the very last parameter passed to the function.
      if (current_token_.kind == TokenKind::kVariadic) {
        if (function_kind != FunctionKind::Extern) {
          HandleError("'...' is only allowed in extern functions");
          return std::nullopt;
        }

        is_variadic = true;
        AdvanceToken();  // skip ...

        if (current_token_.kind != TokenKind::kCloseParen) {
          print_error(text_, current_token_.meta,
                      "'...' must be the last argument in a function");
          return std::nullopt;
        }
        break;
      }

      if (current_token_.kind != TokenKind::kIdent) {
        HandleError("expected parameter name");
        break;
      }

      std::string arg_name = current_token_.value;
      AdvanceToken();  // after parameter name

      if (current_token_.kind != TokenKind::kColon) {
        HandleError("expected ':' after parameter name");
        break;
      }

      AdvanceToken();  // after ':'

      std::optional<ParsedType> arg_type = ParseType();
      if (!arg_type.has_value()) {
        HandleError("expected type name");
        break;
      }

      arguments.emplace_back(std::move(arg_name), std::move(arg_type.value()));

      if (current_token_.kind == TokenKind::kComma) {
        AdvanceToken();
        continue;  // next parameter
      }

      if (current_token_.kind == TokenKind::kCloseParen)
        break;  // done

      HandleError("expected ',' or ')'");
      break;
    }
  }
  Token missing_return_type_token = current_token_;
  AdvanceToken();  // after ')'

  std::optional<ParsedType> return_type;
  if (current_token_.kind == TokenKind::kSkinnyArrow) {
    AdvanceToken();  // consume '->'

    return_type = ParseType();

    // Failed to parse the return type so the function parse is invalid
    if (!return_type.has_value())
      return std::nullopt;

  } else {
    // Using the debug metadata for the last parentheses in the expression
    // ensures that errors with deduced "Void" point to the absence of a return.
    return_type = ParsedType{"Void", missing_return_type_token.meta};
  }

  FunctionDeclaration fn{function_name,
                         std::move(arguments),
                         std::move(return_type.value()),
                         function_kind,
                         is_variadic,
                         Metadata::fromTokens(start_of_arguments_token,
                                              missing_return_type_token)};

  if (current_token_.kind == TokenKind::kEndExpr) {
    AdvanceToken();  // consume ';'
    return std::move(fn);
  }

  if (!ExpectNextToken(TokenKind::kOpenBrace, "expected function body"))
    return std::nullopt;

  Block body;
  ParseBlock(body);
  fn.body = std::make_unique<Block>(std::move(body));
  return std::move(fn);
}

void Parser::AdvanceToken() {
  current_token_ = tokenizer_.next();
  // Always skip comments when parsing the AST
  while (current_token_.kind == TokenKind::kComment)
    current_token_ = tokenizer_.next();
}

std::optional<Token> Parser::ExpectNextToken(TokenKind expected_kind,
                                             std::string_view error_message) {
  if (current_token_.kind != expected_kind) {
    HandleError(error_message);
    return std::nullopt;
  }

  Token expected_token = current_token_;
  AdvanceToken();
  return expected_token;
}

void Parser::HandleError(std::string_view message) {
  print_error(text_, current_token_.meta, message);

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
    AdvanceToken();

  // Sync past ';' as that does not start a statement
  if (current_token_.kind == TokenKind::kEndExpr)
    AdvanceToken();
}
