// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <iostream>
#include <sstream>

// A quick and dirty implementation of LOG() inspired by Chromium's
// base/logging.h and glog but smaller (and lacking features and quality).

namespace core {
namespace logging {

typedef int LogSeverity;

namespace internal {

// Represents a single log message to stdout
class LogMessage final {
 public:
  LogMessage(const char* filename, int line_number, LogSeverity severity);
  ~LogMessage();

  std::ostream& stream() { return stream_; }

 protected:
  // Disallow copy and assign:
  LogMessage(const LogMessage&) = delete;
  LogMessage& operator=(LogMessage&) = delete;

 private:
  const char* filename_;
  const int line_number_;
  const LogSeverity severity_;

  std::ostringstream stream_;
};

// Helper class used to explicitly ignore an std::ostream
class VoidifyStream final {
 public:
  // This operator has lower precedence than << but higher than ?:
  void operator&(std::ostream&) {}
};

// Helper macro which avoids evaluating the arguents to a stream if the
// condition is false.
#define LAZY_CHECK_STREAM(stream, condition) \
  (condition) ? (void)0 : ::core::logging::internal::VoidifyStream() & (stream)

}  // namespace internal

const LogSeverity INFO = 0;
const LogSeverity WARNING = 1;
const LogSeverity ERROR = 2;
const LogSeverity FATAL = 3;

// Allows logging to stdout. LOG(FATAL) will abort() the program.
// Other logging severities are logged but do not affect behavior.
//
// Example:
//   LOG(INFO) << "This is a log to stdout";
#define LOG(severity)                                              \
  ::core::logging::internal::LogMessage(__FILE__, __LINE__,        \
                                        ::core::logging::severity) \
      .stream()

#define LOG_IF(severity, cond) LAZY_CHECK_STREAM(LOG(severity), !(cond))

// Creates a LOG(FATAL), crashing the program, if (expr) is false.
//
// Example:
//   CHECK(0 == 0) << "The universe is not right...";
#define CHECK(expr) \
  LAZY_CHECK_STREAM(LOG(FATAL), (expr)) << "CHECK(" << #expr << ") failed: "
// Operator extenstions to CHECK macro:
#define CHECK_EQ(lhs, rhs) CHECK((lhs) == (rhs))
#define CHECK_NE(lhs, rhs) CHECK((lhs) != (rhs))
#define CHECK_LT(lhs, rhs) CHECK((lhs) < (rhs))
#define CHECK_GT(lhs, rhs) CHECK((lhs) > (rhs))
#define CHECK_OK(expr) CHECK(expr.ok())

// Indicates a point which should not be reached in code.
#define NOTREACHED() LOG(FATAL) << "NOTREACHED(): "

#define COLOR(value) "\u001b[38;5;" << #value << "m"
#define COLOR_RESET() "\u001b[0m"

}  // namespace logging
}  // namespace core