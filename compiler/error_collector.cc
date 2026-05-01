// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/error_collector.h"

#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

void print_error(std::string_view file_contents,
                 Metadata metadata,
                 std::string_view message) {
  size_t line_start = file_contents.rfind('\n', metadata.column_range.start);
  if (line_start == std::string_view::npos)
    line_start = 0;
  else
    ++line_start;

  size_t line_end = file_contents.find('\n', metadata.column_range.start);
  if (line_end == std::string_view::npos)
    line_end = file_contents.size();

  std::string_view line =
      file_contents.substr(line_start, line_end - line_start);
  size_t relative_offset = metadata.column_range.start - line_start;

  std::string kErrorPrefix =
      "Error: " + std::to_string(metadata.line_range.start) + ": ";
  std::cerr << kErrorPrefix << line << std::endl
            << std::setw(kErrorPrefix.length() + relative_offset) << " "
            << "^ " << message << std::endl;
}

}  // namespace

void ErrorCollector::Add(std::string_view message, Metadata meta) {
  errors.push_back({message.data(), std::move(meta)});
}

void ErrorCollector::PrintAllErrors(std::string_view file_contents) const {
  for (const auto& error : errors) {
    print_error(file_contents, error.meta, error.message);
  }
}

bool ErrorCollector::HasErrors() const {
  return !errors.empty();
}