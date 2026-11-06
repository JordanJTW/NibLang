/*
 * Copyright (C) 2026, Jordan Werthman <jordanwerthman@gmail.com>.
 *
 * This software is dual-licensed. You may use this file under the terms of
 * the GNU General Public License v3.0 (GPL-3.0) OR under a commercial
 * license obtained directly from the copyright holder.
 *
 * See the LICENSE.md file in the project root for full licensing details.
 *
 * SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-Commercial
 */

#pragma once

#include <cstddef>
#include <deque>
#include <iostream>
#include <string>

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
  kKwAlias,           // alias
  kKwStatic,          // static
  kKwAs,              // as
  kKwTypeOf,          // of
  kKwNil,             // Nil
  kKwImport,          // @import
  kKwInterface,       // interface
  kKwImplements,      // implements
  kKwSizeOf,          // sizeof
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
  kAnd,               // &
  kPercent,           // %
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
  size_t file_id;

  static Metadata fromTokens(const Token& start, const Token& end);
};

struct Token {
  TokenKind kind;
  std::string value;
  Metadata meta;
};

class Tokenizer {
 public:
  explicit Tokenizer(std::string data, size_t file_id);

  Token next();

 private:
  const std::string data_;
  size_t file_id_;
  size_t offset_{0};

  size_t line_{1};
};

std::ostream& operator<<(std::ostream& os, const Token& token);
std::ostream& operator<<(std::ostream& os, const TokenKind& type);
std::ostream& operator<<(std::ostream& os, const TextRange& range);
std::ostream& operator<<(std::ostream& os, const Metadata& meta);
