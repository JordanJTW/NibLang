// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "compiler/tokenizer.h"
#include "compiler/types.h"

class ErrorCollector {
 public:
  virtual ~ErrorCollector() = default;

  virtual void Add(std::string_view message, Metadata meta);
  virtual void PrintAllErrors(std::string_view file_contents) const;
  virtual bool HasErrors() const;

 private:
  struct Error {
    std::string message;
    Metadata meta;
  };
  std::vector<Error> errors;
};