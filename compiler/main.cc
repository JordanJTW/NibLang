// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "compiler/assembler.h"
#include "compiler/bytecode_generator.h"
#include "compiler/error_collector.h"
#include "compiler/parser.h"
#include "compiler/printer.h"
#include "compiler/program_builder.h"
#include "compiler/semantic_analyzer.h"
#include "compiler/type_context.h"
#include "compiler/types.h"

struct File {
  std::string resolved_path;
  std::vector<std::string> import_paths;
  Block root_block;
  std::string file_contents;
  std::unique_ptr<ErrorCollector> error_collector;
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

  auto error_collector = std::make_unique<ErrorCollector>();
  Block root_block = Parser{file_contents, *error_collector}.Parse();

  std::vector<std::string> import_paths;
  for (const auto& statement : root_block.statements) {
    if (const auto* import = std::get_if<ImportStatement>(&statement->as)) {
      import_paths.push_back(import->path.text);
    }
  }

  return File{std::move(resolved_path), std::move(import_paths),
              std::move(root_block), std::move(file_contents),
              std::move(error_collector)};
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

constexpr std::array<std::string_view, 18> kRuntimeFunctions = {
    "log",
    "fetch",
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

  ScopeManager scope_manager;
  TypeRegistry type_registry{scope_manager};

  bool any_errors = false;
  for (File& file : files) {
    ErrorCollector& error_collector = *file.error_collector;
    TypeContext type_context(scope_manager, type_registry, error_collector);
    SemanticAnalyzer analyzer(type_context, scope_manager, error_collector,
                              type_registry);
    SemanticAnalyzer::FunctionContext context = {{}, TypeRegistry::Any};
    analyzer.Check(file.root_block, context);

    if (error_collector.HasErrors()) {
      std::cerr << "Errors in file: " << file.resolved_path << "\n";
      error_collector.PrintAllErrors(file.file_contents);
      any_errors = true;
    }
  }

  if (opts.mode == OutputMode::Ast) {
    for (const File& file : files)
      Printer(&type_registry).Print(file.root_block);

    return 0;
  }

  if (any_errors)
    return 1;

  ConstantPool constant_pool;
  std::vector<ByteCodeGenerator::FunctionObject> function_objects;
  std::vector<const FunctionSymbol*> external_functions;

  std::set<SymbolId> processed_symbols;
  std::vector<SymbolId> symbols_to_process;

  auto process_symbol = [&](const FunctionSymbol& symbol) {
    // .insert().second returns false if the symbol was already in the set
    if (!processed_symbols.insert(symbol.symbol_id).second) {
      return;
    }

    std::vector<SymbolId> called_symbols;
    if (symbol.IsExtern()) {
      external_functions.push_back(&symbol);
      return;
    }

    ByteCodeGenerator generator{scope_manager, constant_pool};
    function_objects.push_back(
        std::move(generator).Build(symbol, called_symbols));

    for (SymbolId id : called_symbols) {
      symbols_to_process.push_back(id);
    }
  };

  for (const auto& [id, symbol] : type_registry.symbol_table()) {
    if (const auto* fn_symbol = std::get_if<FunctionSymbol>(&symbol)) {
      if (fn_symbol->declaration.name.text == "main") {
        process_symbol(*fn_symbol);
        break;
      }
    }
  }

  while (!symbols_to_process.empty()) {
    SymbolId id = symbols_to_process.back();
    symbols_to_process.pop_back();

    if (processed_symbols.contains(id))
      continue;

    if (const auto* fn_symbol = type_registry.GetSymbol<FunctionSymbol>(id)) {
      process_symbol(*fn_symbol);
    }
  }

  // Output ALL of the instantiated Symbols (without tree-shaking) for debugging
  // for (const auto& [id, symbol] : type_context.symbol_table()) {
  //   if (const auto* fn_symbol = std::get_if<FunctionSymbol>(&symbol)) {
  //     if (fn_symbol->instances.empty())
  //       continue;

  //     if (!fn_symbol->IsExtern()) {
  //       CHECK(fn_symbol->declaration.body);
  //       function_objects.push_back(
  //           ByteCodeGenerator{type_context, scope_manager,
  //           constant_pool}.Build(
  //               *fn_symbol));
  //     } else {
  //       external_functions.push_back(fn_symbol);
  //     }
  //   }
  // }

  ProgramBuilder builder{constant_pool, kRuntimeFunctions};

  std::vector<uint8_t> program_image = builder.GenerateImage(
      std::move(function_objects), std::move(external_functions));

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

  return 0;
}
