// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/error_collector.h"

#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>

namespace {

void print_error(std::string_view path,
                 std::string_view contents,
                 Metadata metadata,
                 std::string_view message) {
  size_t line_start = contents.rfind('\n', metadata.column_range.start);
  if (line_start == std::string_view::npos)
    line_start = 0;
  else
    ++line_start;

  size_t line_end = contents.find('\n', metadata.column_range.start);
  if (line_end == std::string_view::npos)
    line_end = contents.size();

  std::string_view line = contents.substr(line_start, line_end - line_start);
  size_t relative_offset = metadata.column_range.start - line_start;

  std::string kErrorPrefix =
      "Error: " + std::string(path) + ":" +
      std::to_string(metadata.line_range.start) + ":" +
      std::to_string(metadata.column_range.start - line_start + 1) + ":";
  std::cerr << kErrorPrefix << std::endl
            << line << std::endl
            << std::setw(relative_offset) << " "
            << "^ " << message << std::endl;
}

}  // namespace

void ErrorCollector::Add(std::string_view message, Metadata meta) {
  errors.push_back({message.data(), std::move(meta)});
}

void ErrorCollector::PrintAllErrors(std::string_view path,
                                    std::string_view contents) const {
  for (const auto& error : errors) {
    print_error(path, contents, error.meta, error.message);
  }
}

bool ErrorCollector::HasErrors() const {
  return !errors.empty();
}