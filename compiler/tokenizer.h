// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <deque>
#include <iostream>

enum class TokenKind {
  kUnknown = 0,
  kIdent,             // $id, $id1
  kNumber,            // 5, 5.0
  kString,            // "hello world"
  kTemplateString,    // `hello ${world}`
  kChar,              // '\n'
  kKwIf,              // if
  kKwElse,            // else
  kKwFn,              // fn
  kKwTrue,            // true
  kKwFalse,           // false
  kKwReturn,          // return
  kKwThrow,           // throw
  kKwWhile,           // while
  kKwBreak,           // break
  kKwContinue,        // continue
  kKwStruct,          // struct
  kKwExtern,          // extern
  kKwLet,             // let
  kKwStatic,          // static
  kKwAs,              // as
  kKwNil,             // Nil
  kKwImport,          // @import
  kVariadic,          // ...
  kOpenParen,         // (
  kCloseParen,        // )
  kOpenBrace,         // {
  kCloseBrace,        // }
  kSquareOpen,        // [
  kSquareClose,       // ]
  kDot,               // .
  kPipe,              // |
  kComma,             // ,
  kColon,             // :
  kAssign,            // =
  kPlus,              // +
  kMinus,             // -
  kMultiply,          // *
  kDivide,            // /
  kNot,               // !
  kQuestion,          // ?
  kPlusPlus,          // ++
  kMinusMinus,        // --
  kQuestionQuestion,  // ??
  kCompareGt,         // >
  kCompareLt,         // <
  kCompareGe,         // >=
  kCompareLe,         // <=
  kCompareEq,         // ==
  kCompareNe,         // !=
  kSkinnyArrow,       // ->
  kAndAnd,            // &&
  kOrOr,              // ||
  kComment,           // // comment text
  kTokenError,        // Token containing an error message from tokenization
  kEndExpr,           // ;
  kEndOfFile,         // EOF
};

struct TextRange {
  size_t start, end;
};

struct Token;
struct Metadata {
  TextRange column_range;
  TextRange line_range;

  static Metadata fromTokens(const Token& start, const Token& end);
};

struct Token {
  TokenKind kind;
  std::string value;
  Metadata meta;
};

class Tokenizer {
 public:
  explicit Tokenizer(std::string data);

  Token next();

 private:
  const std::string data_;
  size_t offset_{0};

  size_t line_{1};
};

std::ostream& operator<<(std::ostream& os, const Token& token);
std::ostream& operator<<(std::ostream& os, const TokenKind& type);
std::ostream& operator<<(std::ostream& os, const TextRange& range);
std::ostream& operator<<(std::ostream& os, const Metadata& meta);
