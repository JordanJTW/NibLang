#include "compiler/tokenizer.h"

#include <ctype.h>
#include <array>
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
  if (value == "->")
    return TokenKind::kSkinnyArrow;
  return std::nullopt;
}

}  // namespace

Tokenizer::Tokenizer(std::string data) : data_(data) {}

Token Tokenizer::seekTo(const Token& token) {
  offset_ = token.idx;
  return next();
}

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
        .idx = start_idx,
        .length = length,
        .meta = {.line = line_},
        .value = value.empty() ? data_.substr(start_idx, length) : value};
  };

  if (offset_ >= data_.size())
    return make_token(TokenKind::kEndOfFile);

  // Handle keywords
  static constexpr std::array<std::pair<std::string_view, TokenKind>, 14>
      kKeywordToToken{{{"label", TokenKind::kKwLabel},
                       {"goto", TokenKind::kKwGoto},
                       {"if", TokenKind::kKwIf},
                       {"else", TokenKind::kKwElse},
                       {"fn", TokenKind::kKwFn},
                       {"true", TokenKind::kKwTrue},
                       {"false", TokenKind::kKwFalse},
                       {"return", TokenKind::kKwReturn},
                       {"try", TokenKind::kKwTry},
                       {"catch", TokenKind::kKwCatch},
                       {"throw", TokenKind::kKwThrow},
                       {"while", TokenKind::kKwWhile},
                       {"break", TokenKind::kKwBreak},
                       {"continue", TokenKind::kKwContinue}}};

  for (const auto& [keyword, kind] : kKeywordToToken) {
    if (data_.substr(offset_, keyword.size()) == keyword) {
      offset_ += keyword.size();
      return make_token(kind);
    }
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
    std::string value;
    while (offset_ < data_.size() && data_[offset_] != '"') {
      if (data_[offset_] == '\\') {
        ++offset_;  // skip backslash
        char escaped = data_[offset_++];

        switch (escaped) {
          case 'n':
            value.push_back('\n');
            break;
          case 't':
            value.push_back('\t');
            break;
          case 'r':
            value.push_back('\r');
          default:
            value.push_back(escaped);
        }
      } else {
        value.push_back(data_[offset_++]);
      }
    }

    ++offset_;  // Skip final '"'
    return make_token(TokenKind::kString, value);
  }

  // Comment
  if (data_.substr(offset_, 2) == "//") {
    offset_ += 2;  // Skip initial '//'
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
    KIND_TO_NAME(kKwTrue);
    KIND_TO_NAME(kKwFalse);
    KIND_TO_NAME(kKwReturn);
    KIND_TO_NAME(kKwTry);
    KIND_TO_NAME(kKwCatch);
    KIND_TO_NAME(kKwThrow);
    KIND_TO_NAME(kKwWhile);
    KIND_TO_NAME(kKwBreak);
    KIND_TO_NAME(kKwContinue);
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
    KIND_TO_NAME(kSkinnyArrow);
    KIND_TO_NAME(kComment);
    KIND_TO_NAME(kEndExpr);
    KIND_TO_NAME(kEndOfFile);
  }
}

std::ostream& operator<<(std::ostream& os, const Token& token) {
  return os << token.kind << " [" << token.idx << ", "
            << token.idx + token.length << "): \"" << token.value << "\"";
}