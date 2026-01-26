#include "test/ir/tokenizer.h"

#include <ctype.h>
#include <optional>

namespace {

bool isnumber(char ch) {
  return std::isdigit(ch) || ch == '.';
}

std::optional<TokenKind> get_single_char_token(char ch) {
  switch (ch) {
    case '+':
      return TokenKind::kAdd;
    case '-':
      return TokenKind::kSubtract;
    case '*':
      return TokenKind::kMultiply;
    case '/':
      return TokenKind::kDivide;
    case '=':
      return TokenKind::kAssign;
    case ';':
      return TokenKind::kEndExpr;
    case ':':
      return TokenKind::kEndExpr;
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
    default:
      return std::nullopt;
  }
}

std::optional<TokenKind> get_double_char_token(std::string value) {
  if (value == "==")
    return TokenKind::kCompareEq;
  if (value == "!=")
    return TokenKind::kCompareNe;
  if (value == ">=")
    return TokenKind::kCompareGe;
  if (value == "<=")
    return TokenKind::kCompareLe;
  return std::nullopt;
}

}  // namespace

Tokenizer::Tokenizer(std::string data) : data_(data) {}

Token Tokenizer::seekTo(const Token& token) {
  offset_ = token.idx;
  return next();
}

Token Tokenizer::next() {
  while (offset_ < data_.size() && std::isspace(data_[offset_]))
    ++offset_;

  size_t start_idx = offset_;
  auto make_token = [&](TokenKind kind, std::string value = "") {
    size_t length = offset_ - start_idx;
    return Token{
        .kind = kind,
        .idx = start_idx,
        .length = length,
        .value = value.empty() ? data_.substr(start_idx, length) : value};
  };

  if (offset_ >= data_.size())
    return make_token(TokenKind::kEndOfFile);

  // label keyword
  static constexpr std::string_view kLabelKeyword = "label";
  if (data_.substr(offset_, kLabelKeyword.size()) == kLabelKeyword) {
    offset_ += kLabelKeyword.size();
    return make_token(TokenKind::kKwLabel);
  }

  // goto keyword
  static constexpr std::string_view kGotoKeyword = "goto";
  if (data_.substr(offset_, kGotoKeyword.size()) == kGotoKeyword) {
    offset_ += kGotoKeyword.size();
    return make_token(TokenKind::kKwGoto);
  }

  // if keyword
  static constexpr std::string_view kIfKeyword = "if";
  if (data_.substr(offset_, kIfKeyword.size()) == kIfKeyword) {
    offset_ += kIfKeyword.size();
    return make_token(TokenKind::kKwIf);
  }

  // else keyword
  static constexpr std::string_view kElseKeyword = "else";
  if (data_.substr(offset_, kElseKeyword.size()) == kElseKeyword) {
    offset_ += kElseKeyword.size();
    return make_token(TokenKind::kKwElse);
  }

  // fn keyword
  static constexpr std::string_view kFnKeyword = "fn";
  if (data_.substr(offset_, kFnKeyword.size()) == kFnKeyword) {
    offset_ += kFnKeyword.size();
    return make_token(TokenKind::kKwFn);
  }

  // end keyword
  static constexpr std::string_view kEndKeyword = "end";
  if (data_.substr(offset_, kEndKeyword.size()) == kEndKeyword) {
    offset_ += kEndKeyword.size();
    return make_token(TokenKind::kKwEnd);
  }

  char ch = data_[offset_];

  // Identifier
  if (ch == '$' || std::isalpha(ch)) {
    ++offset_;  // Skip initial char
    while (offset_ < data_.size() &&
           (std::isalnum(data_[offset_]) || data_[offset_] == '_'))
      ++offset_;

    return make_token(TokenKind::kIdent);
  }

  // Number (must start with digit but can include '.')
  if (std::isdigit(ch)) {
    while (offset_ < data_.size() && (isnumber(data_[offset_])))
      ++offset_;

    return make_token(TokenKind::kNumber);
  }

  // String
  if (ch == '"') {
    ++offset_;  // Skip initial '"'
    size_t string_length = 0;
    while (offset_ < data_.size() && data_[offset_] != '"') {
      ++string_length;
      ++offset_;
    }

    ++offset_;  // Skip final '"'
    Token token = make_token(TokenKind::kString,
                             data_.substr(start_idx + 1, string_length));
    return token;
  }

  // Comment
  if (ch == '#') {
    while (offset_ < data_.size() && data_[offset_] != '\n')
      ++offset_;

    return make_token(TokenKind::kComment);
  }

  // Handles double character token types (&&||++,etc)
  if (auto kind = get_double_char_token(data_.substr(offset_, 2));
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
    KIND_TO_NAME(kKwLabel);
    KIND_TO_NAME(kKwGoto);
    KIND_TO_NAME(kKwIf);
    KIND_TO_NAME(kKwElse);
    KIND_TO_NAME(kKwFn);
    KIND_TO_NAME(kKwEnd);
    KIND_TO_NAME(kOpenParen);
    KIND_TO_NAME(kCloseParen);
    KIND_TO_NAME(kComma);
    KIND_TO_NAME(kAssign);
    KIND_TO_NAME(kAdd);
    KIND_TO_NAME(kSubtract);
    KIND_TO_NAME(kMultiply);
    KIND_TO_NAME(kDivide);
    KIND_TO_NAME(kCompareGt);
    KIND_TO_NAME(kCompareLt);
    KIND_TO_NAME(kCompareGe);
    KIND_TO_NAME(kCompareLe);
    KIND_TO_NAME(kCompareEq);
    KIND_TO_NAME(kCompareNe);
    KIND_TO_NAME(kComment);
    KIND_TO_NAME(kEndExpr);
    KIND_TO_NAME(kEndOfFile);
  }
}

std::ostream& operator<<(std::ostream& os, const Token& token) {
  return os << token.kind << " [" << token.idx << ", "
            << token.idx + token.length << "): \"" << token.value << "\"";
}