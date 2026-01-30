#include <cassert>
#include <cstdio>
#include <fstream>

#include "compiler/assembler.h"
#include "compiler/parser.h"
#include "compiler/program_builder.h"
#include "compiler/tokenizer.h"
#include "src/vm.h"

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

  ProgramBuilder builder;
  Parser parser(contents, builder);

  parser.Parse();

  std::vector<uint8_t> program_image = builder.GenerateImage();
  std::ofstream out("/tmp/prog.ink", std::ios::binary);

  if (out.is_open()) {
    out.write(reinterpret_cast<const char*>(program_image.data()),
              program_image.size());
    out.close();
  }
  return 0;
}
