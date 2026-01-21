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
    case '=':
      return TokenKind::kAssign;
    case ';':
      return TokenKind::kEndExpr;
    default:
      return std::nullopt;
  }
}

}  // namespace

Tokenizer::Tokenizer(std::string data) : data_(data) {}

Token Tokenizer::next() {
  while (offset_ < data_.size() && std::isspace(data_[offset_]))
    ++offset_;

  size_t start_idx = offset_;
  auto make_token = [&](TokenKind kind) {
    size_t length = offset_ - start_idx;
    return Token{.kind = kind,
                 .idx = start_idx,
                 .length = length,
                 .value = data_.substr(start_idx, length)};
  };

  if (offset_ >= data_.size())
    return make_token(TokenKind::kEndOfFile);

  char ch = data_[offset_];

  // Identifier
  if (ch == '$') {
    ++offset_;  // Skip '$'
    while (offset_ < data_.size() && std::isalnum(data_[offset_]))
      ++offset_;

    return make_token(TokenKind::kIdent);
  }

  // Number (must start with digit but can include '.')
  if (std::isdigit(ch)) {
    while (offset_ < data_.size() && (isnumber(data_[offset_])))
      ++offset_;

    return make_token(TokenKind::kNumber);
  }

  // ASCII character
  if (ch == '\'') {
    ++offset_;  // skip initial '
    while (offset_ < data_.size() && data_[offset_] != '\'')
      ++offset_;

    Token token = make_token(TokenKind::kChar);
    ++offset_;  // Skip final '
    return token;
  }

  // String
  if (ch == '"') {
    start_idx = ++offset_;  // Skip initial '"' (in input/output)
    while (offset_ < data_.size() && data_[offset_] != '"')
      ++offset_;

    Token token = make_token(TokenKind::kString);
    ++offset_;  // Skip final '"'
    return token;
  }

  // Comment
  if (ch == '#') {
    while (offset_ < data_.size() && data_[offset_] != '\n')
      ++offset_;

    return make_token(TokenKind::kComment);
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
    KIND_TO_NAME(kChar);
    KIND_TO_NAME(kString);
    KIND_TO_NAME(kAssign);
    KIND_TO_NAME(kAdd);
    KIND_TO_NAME(kSubtract);
    KIND_TO_NAME(kComment);
    KIND_TO_NAME(kEndExpr);
    KIND_TO_NAME(kEndOfFile);
  }
}

std::ostream& operator<<(std::ostream& os, const Token& token) {
  return os << token.kind << " [" << token.idx << ", "
            << token.idx + token.length << ")";
}