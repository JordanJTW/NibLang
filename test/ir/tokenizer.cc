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
    default:
      return std::nullopt;
  }
}

}  // namespace

Tokenizer::Tokenizer(std::string data) : data_(data) {}

Token Tokenizer::next() {
  while (offset_ < data_.size() && std::isspace(data_[offset_]))
    ++offset_;

  if (offset_ >= data_.size())
    return Token{.kind = TokenKind::kEndOfFile, .idx = offset_, .length = 0};

  char ch = data_[offset_];
  size_t start_idx = offset_;

  // Identifier
  if (ch == '$') {
    ++offset_;  // Skip '$'
    while (offset_ < data_.size() && std::isalnum(data_[offset_]))
      ++offset_;

    return Token{.kind = TokenKind::kIdent,
                 .idx = start_idx,
                 .length = (offset_ - start_idx)};
  }

  // Digits (must start with digit but can include '.')
  if (std::isdigit(ch)) {
    while (offset_ < data_.size() && (isnumber(data_[offset_])))
      ++offset_;

    return Token{.kind = TokenKind::kNumber,
                 .idx = start_idx,
                 .length = (offset_ - start_idx)};
  }

  // Handles single character token types (+-/*=,etc)
  if (auto kind = get_single_char_token(ch); kind.has_value()) {
    ++offset_;
    return Token{.kind = *kind, .idx = start_idx, .length = 1};
  }

  // Skip over an unknown start of token and return it (compiler handles error).
  return Token{.kind = TokenKind::kUnknown, .idx = offset_++, .length = 1};
}

std::ostream& operator<<(std::ostream& os, const TokenKind& type) {
#define KIND_TO_NAME($type) \
  case TokenKind::$type:    \
    return os << #$type
  switch (type) {
    KIND_TO_NAME(kUnknown);
    KIND_TO_NAME(kIdent);
    KIND_TO_NAME(kNumber);
    KIND_TO_NAME(kAssign);
    KIND_TO_NAME(kAdd);
    KIND_TO_NAME(kSubtract);
    KIND_TO_NAME(kEndOfFile);
  }
}

std::ostream& operator<<(std::ostream& os, const Token& token) {
  return os << token.kind << " [" << token.idx << ", "
            << token.idx + token.length << ")";
}