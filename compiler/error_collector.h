#pragma once

#include <string_view>

#include "compiler/types.h"

class ErrorCollector {
 public:
  virtual ~ErrorCollector() = default;
  virtual void Add(std::string_view message, Metadata meta) = 0;
  virtual void PrintAllErrors() const = 0;

  virtual bool HasErrors() const {
    return false;
  }
};

std::unique_ptr<ErrorCollector> DefaultErrorCollector(std::string_view file);