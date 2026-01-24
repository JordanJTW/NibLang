#pragma once

#include <deque>
#include <iostream>

enum class TokenKind {
  kUnknown = 0,
  kIdent,       // $id, $id1
  kNumber,      // 5, 5.0
  kChar,        // 'a', 'b', 'c'
  kString,      // "hello world"
  kKwLabel,     // label
  kKwGoto,      // goto
  kKwIf,        // if
  kKwElse,      // else
  kKwFn,        // fn
  kOpenParen,   // (
  kCloseParen,  // )
  kComma,       // ,
  kAssign,      // =
  kAdd,         // +
  kSubtract,    // -
  kMultiply,    // *
  kDivide,      // /
  kCompareGt,   // >
  kCompareLt,   // <
  kCompareGe,   // >=
  kCompareLe,   // <=
  kCompareEq,   // ==
  kCompareNe,   // !=
  kComment,     // # comment
  kEndExpr,     // ;
  kEndOfFile,   // EOF
};

struct Token {
  TokenKind kind;
  size_t idx, length;  // Where is the Token's text located
  std::string value;
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
