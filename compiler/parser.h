#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "compiler/program_builder.h"
#include "compiler/tokenizer.h"
#include "compiler/types.h"

class Parser {
 public:
  explicit Parser(const std::string& text)
      : text_(text), tokenizer_(text_), current_token_(tokenizer_.next()) {}

  Block Parse();

 private:
  void ParseBlock(Block& block);

  std::unique_ptr<Expression> ParseExpression();
  std::unique_ptr<Expression> ParseAssignment();
  std::unique_ptr<Expression> ParseLogical();
  std::unique_ptr<Expression> ParseComparison();
  std::unique_ptr<Expression> ParseAdditive();
  std::unique_ptr<Expression> ParseMultiplicative();
  std::unique_ptr<Expression> ParseUnary();
  std::unique_ptr<Expression> ParsePostFix();
  std::unique_ptr<Expression> ParsePrimary();
  std::unique_ptr<Expression> ParseValue(const Token& value);

  std::unique_ptr<Expression> ParseCall(std::unique_ptr<Expression> callee);

  std::optional<ParsedType> ParseType();
  enum class ExternStruct { YES, NO };
  std::optional<StructDeclaration> ParseStructDeclaration(
      ExternStruct is_extern);
  std::optional<FunctionDeclaration> ParseFunctionDeclaration(
      FunctionKind function_kind);

  std::optional<Token> ExpectNextToken(TokenKind expected_kind,
                                       std::string_view error_message);
  void AdvanceToken();
  void HandleError(std::string_view message);

  const std::string& text_;
  Tokenizer tokenizer_;
  Token current_token_;
};