/*
 * Copyright (C) 2026, Jordan Werthman <jordanwerthman@gmail.com>.
 *
 * This software is dual-licensed. You may use this file under the terms of
 * the GNU General Public License v3.0 (GPL-3.0) OR under a commercial
 * license obtained directly from the copyright holder.
 *
 * See the LICENSE.md file in the project root for full licensing details.
 *
 * SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-Commercial
 */

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

  CHECK_LE(line_start, metadata.column_range.start)
      << "Error: '" << message << "' has invalid span";
  size_t relative_offset = metadata.column_range.start - line_start;
  CHECK_LE(metadata.column_range.start, metadata.column_range.end)
      << "Error: '" << message << "' has invalid span";
  size_t span_length = metadata.column_range.end - metadata.column_range.start;

  std::string header = std::string(prefix) + ": " + std::string(path) + ":" +
                       std::to_string(metadata.line_range.start) + ":" +
                       std::to_string(relative_offset + 1) + ":";

  std::string underline_line(relative_offset, ' ');
  underline_line.push_back('^');
  if (span_length > 1)
    underline_line.append(span_length - 1, '~');

  std::cerr << header << "\n"
            << line << "\n"
            << underline_line << " " << message << "\n";
}

}  // namespace

ErrorBuilder ErrorCollector::Add(std::string message,
                                 Metadata span,
                                 Location location) {
  return ErrorBuilder(*this, std::move(message), span, std::move(location));
}

void ErrorCollector::PrintAllErrors(const std::vector<File>& files) const {
  std::vector<const File*> ordered_files(files.size(), nullptr);
  for (const File& file : files)
    ordered_files[file.file_id] = &file;

  for (const auto& error : errors) {
    LOG(INFO) << "From: " << error.location.path << ":" << error.location.line;

    const File* file = ordered_files[error.message.span.file_id];
    print_error(file->resolved_path, file->file_contents, error.message.span,
                error.message.text, "Error");

    for (const auto& message : error.notes) {
      const File* file = ordered_files[message.span.file_id];
      print_error(file->resolved_path, file->file_contents, message.span,
                  message.text, "Note");
    }
    std::cerr << std::endl;
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