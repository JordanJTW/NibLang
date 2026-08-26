// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "logging.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace core {
namespace logging {

static bool s_logging_enabled = true;

namespace internal {
namespace {

const std::string GetName(LogSeverity severity) {
#define NAME_STATEMENT(value) \
  case value:                 \
    return #value;
  switch (severity) {
    NAME_STATEMENT(INFO);
    NAME_STATEMENT(WARNING);
    NAME_STATEMENT(ERROR);
    NAME_STATEMENT(FATAL);
    default:
      break;
  }
  NOTREACHED();
  return {};
#undef NAME_STATEMENT
}

std::string_view GetFileName(std::string_view filepath) {
  return filepath.substr(filepath.rfind('/') + 1);
}

[[noreturn]] inline void ImmediateCrash() {
  __builtin_trap();
}

}  // namespace

LogMessage::LogMessage(const char* filename,
                       int line_number,
                       LogSeverity severity)
    : filename_(filename), line_number_(line_number), severity_(severity) {
  stream_ << "[" << GetName(severity_) << "] " << GetFileName(filename_) << ":"
          << line_number_ << ": ";
}

LogMessage::~LogMessage() {
  if (!s_logging_enabled)
    return;

  fprintf(stderr, "%s\n", stream_.str().c_str());
  if (severity_ == FATAL) {
    ImmediateCrash();
  }
}

}  // namespace internal

void DisableLogging() {
  s_logging_enabled = false;
}

}  // namespace logging
}  // namespace core