#pragma once

#include <deque>
#include <iostream>

enum class TokenKind {
  kUnknown = 0,
  kIdent,        // $id, $id1
  kNumber,       // 5, 5.0
  kString,       // "hello world"
  kKwLabel,      // label
  kKwGoto,       // goto
  kKwIf,         // if
  kKwElse,       // else
  kKwFn,         // fn
  kKwTrue,       // true
  kKwFalse,      // false
  kKwReturn,     // return
  kKwTry,        // try
  kKwCatch,      // catch
  kKwThrow,      // throw
  kKwWhile,      // while
  kKwBreak,      // break
  kKwContinue,   // continue
  kOpenParen,    // (
  kCloseParen,   // )
  kOpenBrace,    // {
  kCloseBrace,   // }
  kSquareOpen,   // [
  kSquareClose,  // ]
  kDot,          // .
  kPipe,         // |
  kComma,        // ,
  kColon,        // :
  kAssign,       // =
  kAdd,          // +
  kSubtract,     // -
  kMultiply,     // *
  kDivide,       // /
  kCompareGt,    // >
  kCompareLt,    // <
  kCompareGe,    // >=
  kCompareLe,    // <=
  kCompareEq,    // ==
  kCompareNe,    // !=
  kSkinnyArrow,  // ->
  kAndAnd,       // &&
  kOrOr,         // ||
  kComment,      // // comment text
  kEndExpr,      // ;
  kEndOfFile,    // EOF
};

struct MetaInfo {
  size_t line;
};

struct Token {
  TokenKind kind;
  size_t idx, length;  // Where is the Token's text located
  std::string value;
  MetaInfo meta;
};

class Tokenizer {
 public:
  explicit Tokenizer(std::string data);

  Token next();
  Token seekTo(const Token& token);

 private:
  const std::string data_;
  size_t offset_{0};

  size_t line_{1};
};

std::ostream& operator<<(std::ostream& os, const Token& token);
std::ostream& operator<<(std::ostream& os, const TokenKind& type);
