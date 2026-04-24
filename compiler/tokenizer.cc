#include "compiler/tokenizer.h"

#include <ctype.h>
#include <array>
#include <optional>

#include "compiler/logging.h"

namespace {

bool isnumber(char ch) {
  return std::isdigit(ch) || ch == '.';
}

std::optional<TokenKind> get_single_char_token(char ch) {
  switch (ch) {
    case '+':
      return TokenKind::kPlus;
    case '-':
      return TokenKind::kMinus;
    case '*':
      return TokenKind::kMultiply;
    case '/':
      return TokenKind::kDivide;
    case '=':
      return TokenKind::kAssign;
    case ';':
      return TokenKind::kEndExpr;
    case ':':
      return TokenKind::kColon;
    case ',':
      return TokenKind::kComma;
    case '>':
      return TokenKind::kCompareGt;
    case '<':
      return TokenKind::kCompareLt;
    case '(':
      return TokenKind::kOpenParen;
    case ')':
      return TokenKind::kCloseParen;
    case '{':
      return TokenKind::kOpenBrace;
    case '}':
      return TokenKind::kCloseBrace;
    case '[':
      return TokenKind::kSquareOpen;
    case ']':
      return TokenKind::kSquareClose;
    case '.':
      return TokenKind::kDot;
    case '|':
      return TokenKind::kPipe;
    case '!':
      return TokenKind::kNot;
    case '?':
      return TokenKind::kQuestion;
    default:
      return std::nullopt;
  }
}

std::optional<TokenKind> get_double_char_token(std::string_view value) {
  if (value == "==")
    return TokenKind::kCompareEq;
  if (value == "!=")
    return TokenKind::kCompareNe;
  if (value == ">=")
    return TokenKind::kCompareGe;
  if (value == "<=")
    return TokenKind::kCompareLe;
  if (value == "->")
    return TokenKind::kSkinnyArrow;
  if (value == "&&")
    return TokenKind::kAndAnd;
  if (value == "||")
    return TokenKind::kOrOr;
  if (value == "++")
    return TokenKind::kPlusPlus;
  if (value == "--")
    return TokenKind::kMinusMinus;
  if (value == "??")
    return TokenKind::kQuestionQuestion;
  return std::nullopt;
}

}  // namespace

Tokenizer::Tokenizer(std::string data) : data_(data) {}

Token Tokenizer::next() {
  while (offset_ < data_.size() && std::isspace(data_[offset_])) {
    if (data_[offset_] == '\n') {
      ++line_;
    }
    ++offset_;
  }

  size_t start_idx = offset_;
  auto make_token = [&](TokenKind kind, std::string value = "") {
    size_t length = offset_ - start_idx;
    return Token{
        .kind = kind,
        .value = value.empty() ? data_.substr(start_idx, length) : value,
        Metadata{TextRange{start_idx, offset_}, TextRange{line_, line_ + 1}}};
  };

  if (offset_ >= data_.size())
    return make_token(TokenKind::kEndOfFile);

  // Handle keywords
  static constexpr std::array<std::pair<std::string_view, TokenKind>, 17>
      kKeywordToToken{{{"if", TokenKind::kKwIf},
                       {"else", TokenKind::kKwElse},
                       {"fn", TokenKind::kKwFn},
                       {"true", TokenKind::kKwTrue},
                       {"false", TokenKind::kKwFalse},
                       {"return", TokenKind::kKwReturn},
                       {"throw", TokenKind::kKwThrow},
                       {"while", TokenKind::kKwWhile},
                       {"break", TokenKind::kKwBreak},
                       {"continue", TokenKind::kKwContinue},
                       {"struct", TokenKind::kKwStruct},
                       {"extern", TokenKind::kKwExtern},
                       {"let", TokenKind::kKwLet},
                       {"static", TokenKind::kKwStatic},
                       {"as", TokenKind::kKwAs},
                       {"Nil", TokenKind::kKwNil},
                       {"@import", TokenKind::kKwImport}}};

  char ch = data_[offset_];

  // Identifier
  if (ch == '$' || ch == '@' || std::isalpha(ch)) {
    ++offset_;  // Skip initial char
    while (offset_ < data_.size() &&
           (std::isalnum(data_[offset_]) || data_[offset_] == '_'))
      ++offset_;

    std::string_view ident(data_.data() + start_idx, offset_ - start_idx);
    for (const auto& [keyword, kind] : kKeywordToToken) {
      if (ident == keyword)
        return make_token(kind);
    }

    return make_token(TokenKind::kIdent);
  }

  // Number (must start with digit but can include '.')
  if (std::isdigit(ch)) {
    while (offset_ < data_.size() && (isnumber(data_[offset_])))
      ++offset_;

    return make_token(TokenKind::kNumber);
  }

  auto make_string_token = [&](TokenKind kind, char endpoint) {
    ++offset_;  // Skip opening char

    size_t start = offset_;
    while (offset_ < data_.size()) {
      char ch = data_[offset_++];
      if (ch == '\n')
        break;

      if (ch == '\\') {
        if (offset_ >= data_.size())
          return make_token(TokenKind::kTokenError, "unterminated escape");

        ++offset_;  // skip escaped char
      } else if (ch == endpoint) {
        size_t length = offset_ - start - 1;  // exclude endpoint
        return make_token(kind, data_.substr(start, length));
      }
    }
    std::cout << "unterminated!" << std::endl;
    return make_token(TokenKind::kTokenError, "unterminated literal");
  };

  // String
  if (ch == '"') {
    return make_string_token(TokenKind::kString, '"');
  }

  // Template String
  if (ch == '`') {
    return make_string_token(TokenKind::kTemplateString, '`');
  }

  // Char
  if (ch == '\'') {
    return make_string_token(TokenKind::kChar, '\'');
  }

  if (std::string_view{data_.data() + offset_, 3} == "...") {
    offset_ += 3;
    return make_token(TokenKind::kVariadic);
  }

  // Comment
  if (data_.substr(offset_, 2) == "//") {
    offset_ += 2;  // Skip initial '//'
    while (offset_ < data_.size() && data_[offset_] != '\n')
      ++offset_;

    return make_token(TokenKind::kComment);
  }

  // Handles double character token types (&&||++,etc)
  if (auto kind = get_double_char_token({data_.data() + offset_, 2});
      kind.has_value()) {
    offset_ += 2;
    return make_token(*kind);
  }

  // Handles single character token types (+-/*=,etc)
  if (auto kind = get_single_char_token(ch); kind.has_value()) {
    ++offset_;
    return make_token(*kind);
  }

  ++offset_;  // Skip over unknown
  return make_token(TokenKind::kUnknown);
}

std::ostream& operator<<(std::ostream& os, const TokenKind& type) {
#define KIND_TO_NAME($type) \
  case TokenKind::$type:    \
    return os << #$type
  switch (type) {
    KIND_TO_NAME(kUnknown);
    KIND_TO_NAME(kIdent);
    KIND_TO_NAME(kNumber);
    KIND_TO_NAME(kString);
    KIND_TO_NAME(kTemplateString);
    KIND_TO_NAME(kChar);
    KIND_TO_NAME(kKwIf);
    KIND_TO_NAME(kKwElse);
    KIND_TO_NAME(kKwFn);
    KIND_TO_NAME(kKwTrue);
    KIND_TO_NAME(kKwFalse);
    KIND_TO_NAME(kKwReturn);
    KIND_TO_NAME(kKwThrow);
    KIND_TO_NAME(kKwWhile);
    KIND_TO_NAME(kKwBreak);
    KIND_TO_NAME(kKwContinue);
    KIND_TO_NAME(kKwStruct);
    KIND_TO_NAME(kKwExtern);
    KIND_TO_NAME(kKwLet);
    KIND_TO_NAME(kKwStatic);
    KIND_TO_NAME(kKwAs);
    KIND_TO_NAME(kKwNil);
    KIND_TO_NAME(kKwImport);
    KIND_TO_NAME(kVariadic);
    KIND_TO_NAME(kOpenParen);
    KIND_TO_NAME(kCloseParen);
    KIND_TO_NAME(kOpenBrace);
    KIND_TO_NAME(kCloseBrace);
    KIND_TO_NAME(kSquareOpen);
    KIND_TO_NAME(kSquareClose);
    KIND_TO_NAME(kDot);
    KIND_TO_NAME(kPipe);
    KIND_TO_NAME(kComma);
    KIND_TO_NAME(kColon);
    KIND_TO_NAME(kAssign);
    KIND_TO_NAME(kPlus);
    KIND_TO_NAME(kMinus);
    KIND_TO_NAME(kMultiply);
    KIND_TO_NAME(kDivide);
    KIND_TO_NAME(kNot);
    KIND_TO_NAME(kQuestion);
    KIND_TO_NAME(kPlusPlus);
    KIND_TO_NAME(kMinusMinus);
    KIND_TO_NAME(kQuestionQuestion);
    KIND_TO_NAME(kCompareGt);
    KIND_TO_NAME(kCompareLt);
    KIND_TO_NAME(kCompareGe);
    KIND_TO_NAME(kCompareLe);
    KIND_TO_NAME(kCompareEq);
    KIND_TO_NAME(kCompareNe);
    KIND_TO_NAME(kSkinnyArrow);
    KIND_TO_NAME(kAndAnd);
    KIND_TO_NAME(kOrOr);
    KIND_TO_NAME(kComment);
    KIND_TO_NAME(kTokenError);
    KIND_TO_NAME(kEndExpr);
    KIND_TO_NAME(kEndOfFile);
  }
}

// static
Metadata Metadata::fromTokens(const Token& start, const Token& end) {
  return Metadata{
      TextRange{start.meta.column_range.start, end.meta.column_range.end},
      TextRange{start.meta.line_range.start, end.meta.line_range.end}};
}

std::ostream& operator<<(std::ostream& os, const TextRange& range) {
  return os << "[" << range.start << ", " << range.end << ")";
}

std::ostream& operator<<(std::ostream& os, const Metadata& meta) {
  return os << "Column: " << meta.column_range << " Line: " << meta.line_range;
}

std::ostream& operator<<(std::ostream& os, const Token& token) {
  return os << token.kind << "\"" << token.value << "\" " << token.meta;
}