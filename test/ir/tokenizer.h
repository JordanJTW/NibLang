#pragma once

#include <iostream>

enum class TokenKind {
  kUnknown = 0,
  kIdent,      // $id, $id1
  kNumber,     // 5, 5.0
  kAssign,     // =
  kAdd,        // +
  kSubtract,   // -
  kEndOfFile,  // EOF
};

struct Token {
  TokenKind kind;
  size_t idx, length;  // Where is the Token's text located
};

class Tokenizer {
 public:
  explicit Tokenizer(std::string data);

  Token next();

 private:
  const std::string data_;
  size_t offset_{0};
};

std::ostream& operator<<(std::ostream& os, const Token& token);
std::ostream& operator<<(std::ostream& os, const TokenKind& type);
