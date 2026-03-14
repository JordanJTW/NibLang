#include "compiler/error_collector.h"

#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

void print_error(const std::string& file,
                 Metadata metadata,
                 std::string_view message) {
  size_t line_start = file.rfind('\n', metadata.column_range.start);
  if (line_start == std::string_view::npos)
    line_start = 0;
  else
    ++line_start;

  size_t line_end = file.find('\n', metadata.column_range.start);
  if (line_end == std::string_view::npos)
    line_end = file.size();

  std::string line = file.substr(line_start, line_end - line_start);
  size_t relative_offset = metadata.column_range.start - line_start;

  std::string kErrorPrefix =
      "Error: " + std::to_string(metadata.line_range.start) + ": ";
  std::cerr << kErrorPrefix << line << std::endl
            << std::setw(kErrorPrefix.length() + relative_offset) << " "
            << "^ " << message << std::endl;
}

class ErrorCollectorImpl : public ErrorCollector {
 public:
  ErrorCollectorImpl(std::string_view file) : file_(file.data()) {}

  void Add(std::string_view message, Metadata meta) override {
    errors.push_back({message.data(), std::move(meta)});
  }

  void PrintAllErrors() const override {
    for (const auto& error : errors) {
      print_error(file_, error.meta, error.message);
    }
  }

  bool HasErrors() const override {
    return !errors.empty();
  }

 private:
  const std::string file_;

  struct Error {
    std::string message;
    Metadata meta;
  };
  std::vector<Error> errors;
};

}  // namespace

std::unique_ptr<ErrorCollector> DefaultErrorCollector(std::string_view file) {
  return std::make_unique<ErrorCollectorImpl>(file);
}