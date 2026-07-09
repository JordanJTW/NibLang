// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/error_collector.h"

#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>

#include "compiler/logging.h"

namespace {

void print_error(std::string_view path,
                 std::string_view contents,
                 Metadata metadata,
                 std::string_view message,
                 std::string_view prefix) {
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

  std::string kPrefix =
      std::string(prefix) + ": " + std::string(path) + ":" +
      std::to_string(metadata.line_range.start) + ":" +
      std::to_string(metadata.column_range.start - line_start + 1) + ":";
  std::cerr << kPrefix << std::endl
            << line << std::endl
            << std::setw(relative_offset) << " "
            << "^ " << message << std::endl;
}

}  // namespace

ErrorBuilder ErrorCollector::Add(std::string message, Metadata span) {
  return ErrorBuilder(*this, std::move(message), span);
}

void ErrorCollector::PrintAllErrors(const std::vector<File>& files) const {
  std::vector<const File*> ordered_files(files.size(), nullptr);
  for (const File& file : files)
    ordered_files[file.file_id] = &file;

  for (const auto& error : errors) {
    const File* file = ordered_files[error.message.span.file_id];
    print_error(file->resolved_path, file->file_contents, error.message.span,
                error.message.text, "Error");

    for (const auto& message : error.notes) {
      const File* file = ordered_files[message.span.file_id];
      print_error(file->resolved_path, file->file_contents, message.span,
                  message.text, "Note");
    }
  }
}

bool ErrorCollector::HasErrors() const {
  return !errors.empty();
}

void ErrorCollector::Commit(Error error) {
  errors.push_back(std::move(error));
}

ErrorBuilder::~ErrorBuilder() {
  error_collector_.Commit(std::move(error_));
}