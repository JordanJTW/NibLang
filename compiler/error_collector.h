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

#pragma once

#include <string>
#include <vector>

#include "compiler/file.h"
#include "compiler/parser/tokenizer.h"

class ErrorBuilder;

struct Message {
  std::string text;
  Metadata span;
};

struct Error {
  Message message;
  std::vector<Message> notes;
};

class ErrorCollector {
 public:
  virtual ~ErrorCollector() = default;

  virtual ErrorBuilder Add(std::string message, Metadata meta);
  virtual void PrintAllErrors(const std::vector<File>& files) const;
  virtual bool HasErrors() const;

 private:
  friend class ErrorBuilder;
  void Commit(Error error);

  std::vector<Error> errors;
};

// Fluent builder for composing `Error`.
class ErrorBuilder {
 public:
  ErrorBuilder(ErrorCollector& error_collector,
               std::string message,
               Metadata span)
      : error_collector_(error_collector),
        error_({std::move(message), span}, {}) {}
  ~ErrorBuilder();

  ErrorBuilder(const ErrorBuilder&) = delete;
  ErrorBuilder& operator=(const ErrorBuilder&) = delete;
  ErrorBuilder(ErrorBuilder&&) = delete;
  ErrorBuilder& operator=(ErrorBuilder&&) = delete;

  ErrorBuilder&& WithNote(std::string message, Metadata span) && {
    error_.notes.push_back({std::move(message), span});
    return std::move(*this);
  }

 private:
  ErrorCollector& error_collector_;
  Error error_;
};