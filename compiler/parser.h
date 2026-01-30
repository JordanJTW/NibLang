#pragma once

#include <optional>
#include <string>

#include "compiler/program_builder.h"
#include "compiler/tokenizer.h"

class Parser {
 public:
  explicit Parser(const std::string& text, ProgramBuilder& builder)
      : text_(text), tokenizer_(text_), builder_(builder) {
    builder_.EnterFunctionScope("(main)", /*arguments=*/{});
  }

  void Parse();

 private:
  bool EmitValue(const Token& value);
  void EmitOp(const Token& op);
  std::optional<Token> ParseFunction(Token& current_token,
                                     std::vector<Token>& args);

  bool ParseExpression(Token& current_token);
  bool ParseAssignment(Token& current_token);
  bool ParseComparison(Token& current_token);
  bool ParseMultiplicative(Token& current_token);
  bool ParseAdditive(Token& current_token);
  bool ParseValue(Token& current_token);
  bool ParseCall(Token& current_token, Token fn_name);

  const std::string& text_;
  Tokenizer tokenizer_;
  ProgramBuilder& builder_;
};