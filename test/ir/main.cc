#include "test/ir/tokenizer.h"

#include <cstdio>
#include <fstream>
#include <iomanip>

void print_error(const std::string& file, Token token) {
  size_t line_start = file.rfind('\n', token.idx);
  if (line_start == std::string_view::npos)
    line_start = 0;
  else
    ++line_start;

  size_t line_end = file.find('\n', token.idx);
  if (line_end == std::string_view::npos)
    line_end = file.size();

  std::string line = file.substr(line_start, line_end - line_start);
  size_t relative_offset = token.idx - line_start;

  constexpr std::string_view kErrorPrefix = "Error: ";
  std::cerr << kErrorPrefix << line << std::endl
            << std::setw(kErrorPrefix.length() + relative_offset) << " "
            << "^ unknown token" << std::endl;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    fprintf(stderr, "%s <path>\n", argv[0]);
    return 1;
  }

  std::ifstream file(argv[1]);
  if (!file) {
    fprintf(stderr, "Error opening: %s\n", argv[1]);
    return 1;
  }

  std::string contents{std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>()};

  Tokenizer tokenizer(contents);

  for (;;) {
    Token token = tokenizer.next();

    if (token.kind == TokenKind::kEndOfFile)
      break;

    if (token.kind == TokenKind::kUnknown) {
      print_error(contents, token);
      continue;
    }

    std::cout << token << std::endl;
  }
  return 0;
}