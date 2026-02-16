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
  explicit Parser(const std::string& text, ProgramBuilder& builder)
      : text_(text), tokenizer_(text_), builder_(builder) {}

  Block Parse();

 private:
  void ParseBlock(Token& token, Block& block);

  std::unique_ptr<Expression> ParseExpression(Token& current_token);
  std::unique_ptr<Expression> ParseAssignment(Token& current_token);
  std::unique_ptr<Expression> ParseLogical(Token& current_token);
  std::unique_ptr<Expression> ParseComparison(Token& current_token);
  std::unique_ptr<Expression> ParseAdditive(Token& current_token);
  std::unique_ptr<Expression> ParseMultiplicative(Token& current_token);
  std::unique_ptr<Expression> ParsePostFix(Token& current_token);
  std::unique_ptr<Expression> ParsePrimary(Token& current_token);
  std::unique_ptr<Expression> ParseValue(const Token& value);

  std::unique_ptr<Expression> ParseCall(Token& current_token,
                                        std::unique_ptr<Expression> callee);

  void HandleError(Token& error_token, const std::string& message);

  const std::string& text_;
  Tokenizer tokenizer_;
  ProgramBuilder& builder_;
};