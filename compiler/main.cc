// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <cassert>
#include <cstdio>
#include <fstream>
#include <unordered_set>

#include "compiler/assembler.h"
#include "compiler/bytecode_generator.h"
#include "compiler/logging.h"
#include "compiler/parser.h"
#include "compiler/printer.h"
#include "compiler/program_builder.h"
#include "compiler/semantic_analyzer.h"
#include "compiler/tokenizer.h"
#include "compiler/type_context.h"
#include "src/vm.h"

struct File {
  std::string resolved_path;
  std::vector<std::string> import_paths;
  Block root_block;
  std::string file_contents;
};

std::string OpenFileWithFallback(std::ifstream& file,
                                 std::string_view path,
                                 std::string_view search_path) {
  file.open(path.data());
  if (file)
    return path.data();

  file.clear();

  size_t start = 0;
  while (start <= search_path.size()) {
    size_t end = search_path.find(';', start);
    if (end == std::string::npos)
      end = search_path.size();

    std::string_view prefix(search_path.data() + start, end - start);

    if (!prefix.empty()) {
      std::filesystem::path candidate = std::filesystem::path(prefix) / path;

      file.open(candidate);
      if (file)
        return candidate.string();

      file.clear();
    }

    start = end + 1;
  }

  fprintf(stderr, "Error opening import: \"%s\"\n", path.data());
  return {};
}

std::optional<File> CollectImportsFor(std::string_view path,
                                      std::string_view search_path) {
  std::ifstream file_to_compile;
  std::string resolved_path =
      OpenFileWithFallback(file_to_compile, path, search_path);

  std::string file_contents{std::istreambuf_iterator<char>(file_to_compile),
                            std::istreambuf_iterator<char>()};

  Block root_block = Parser{file_contents}.Parse();

  std::vector<std::string> import_paths;
  for (const auto& statement : root_block.statements) {
    if (const auto* import = std::get_if<ImportStatement>(&statement->as)) {
      import_paths.push_back(import->path);
    }
  }

  return File{std::move(resolved_path), std::move(import_paths),
              std::move(root_block), std::move(file_contents)};
}

std::vector<File> CalculateImportsFor(std::string_view path,
                                      std::string_view search_path) {
  std::vector<File> result;
  std::unordered_set<std::string> visited;

  std::function<void(const std::string&)> dfs =
      [&](const std::string& current_path) {
        if (visited.find(current_path) != visited.end())
          return;

        visited.insert(current_path);

        auto file_opt = CollectImportsFor(current_path, search_path);
        if (!file_opt)
          return;

        File file = std::move(*file_opt);
        for (const auto& import_path : file.import_paths)
          dfs(import_path);

        result.push_back(std::move(file));
      };

  dfs(std::string(path));
  return result;
}
enum class OutputMode { Ast, DumpImage, Image };

struct Options {
  OutputMode mode = OutputMode::Image;
  std::string input_path;
  std::string output_path;
};

Options ParseArgs(int argc, char* argv[]) {
  Options opts;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "--ast") {
      opts.mode = OutputMode::Ast;
    } else if (arg == "--dump") {
      opts.mode = OutputMode::DumpImage;
    } else if (arg == "--output" && i + 1 < argc) {
      opts.output_path = argv[++i];
    } else {
      opts.input_path = arg;
    }
  }

  if (opts.input_path.empty()) {
    fprintf(stderr, "Usage: %s [--ast|--dump] <file>\n", argv[0]);
    exit(1);
  }

  return opts;
}

constexpr std::array<std::string_view, 19> kRuntimeFunctions = {
    "log",
    "fetch",
    "String_valueOf",
    "Font_load",
    "Font_drawText",
    "Font_calculateSpan",
    "Screen_width",
    "Screen_height",
    "Screen_clear",
    "setTimeout",
    "setInterval",
    "clearTimeout",
    "DateTime_now",
    "DateTime_year",
    "DateTime_month",
    "DateTime_day",
    "DateTime_hour",
    "DateTime_minute",
    "DateTime_second",
};

int main(int argc, char* argv[]) {
  Options opts = ParseArgs(argc, argv);

  char* env_search_path = std::getenv("NIB_PATH");

  if (env_search_path == NULL) {
    fprintf(stderr, "Missing NIB_PATH=\"path/to/std\"\n");
    return 1;
  }

  std::vector<File> files =
      CalculateImportsFor(opts.input_path, env_search_path);

  TypeContext type_context(kRuntimeFunctions);

  bool any_errors = false;
  for (File& file : files) {
    ErrorCollector error_collector;
    SemanticAnalyzer analyzer(type_context, error_collector);
    analyzer.Check(file.root_block);

    if (error_collector.HasErrors()) {
      std::cerr << "Errors in file: " << file.resolved_path << "\n";
      error_collector.PrintAllErrors(file.file_contents);
      any_errors = true;
    }
  }

  if (opts.mode == OutputMode::Ast) {
    for (const File& file : files)
      Printer(&type_context).Print(file.root_block);

    return 0;
  }

  if (any_errors)
    return 1;

  ProgramBuilder builder;

  ByteCodeGenerator bytecode_generator{builder, type_context};
  for (const File& file : files) {
    bytecode_generator.EmitBlock(file.root_block);
  }
  builder.GetCurrentCode().Return();

  std::vector<uint8_t> program_image = builder.GenerateImage();

  if (opts.mode == OutputMode::DumpImage) {
    return ProgramBuilder::DumpImage(program_image) ? 0 : -1;
  }

  if (opts.output_path.empty()) {
    std::cout.write(reinterpret_cast<const char*>(program_image.data()),
                    program_image.size());
    std::cout.flush();
  } else {
    std::ofstream out(opts.output_path, std::ios::binary);
    if (out.is_open()) {
      out.write(reinterpret_cast<const char*>(program_image.data()),
                program_image.size());
      out.close();
    }
  }

  // std::ifstream file(opts.input_path);
  // if (!file) {
  //   fprintf(stderr, "Error opening: %s\n", opts.input_path.c_str());
  //   return 1;
  // }

  // std::string contents{std::istreambuf_iterator<char>(file),
  //                      std::istreambuf_iterator<char>()};

  // Parser parser(contents);

  // Block root = parser.Parse();

  // ProgramBuilder builder;

  // auto error_collector = DefaultErrorCollector(contents);

  // TypeContext type_context;
  // SemanticAnalyzer analyzer(type_context, *error_collector);
  // analyzer.Check(root);

  // if (error_collector->HasErrors()) {
  //   error_collector->PrintAllErrors();
  //   return 1;
  // }

  // if (opts.mode == OutputMode::Ast) {
  //   Printer(&type_context).Print(root);
  //   return 0;
  // }

  // compile(root, builder, type_context);
  // builder.GetCurrentCode().Return();

  // std::vector<uint8_t> program_image = builder.GenerateImage();

  // if (opts.mode == OutputMode::DumpImage) {
  //   return DumpImage(program_image.data(), program_image.size());
  // }

  // if (opts.output_path.empty()) {
  //   std::cout.write(reinterpret_cast<const char*>(program_image.data()),
  //                   program_image.size());
  //   std::cout.flush();
  // } else {
  //   std::ofstream out(opts.output_path, std::ios::binary);
  //   if (out.is_open()) {
  //     out.write(reinterpret_cast<const char*>(program_image.data()),
  //               program_image.size());
  //     out.close();
  //   }
  // }
  return 0;
}
