// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stack>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "compiler/assembler.h"
#include "compiler/bytecode_generator.h"
#include "compiler/types.h"

class ProgramBuilder {
 public:
  explicit ProgramBuilder(
      const ConstantPool& constant_pool,
      std::span<const std::string_view> external_functions = {});

  std::vector<uint8_t> GenerateImage(
      std::vector<ByteCodeGenerator::FunctionObject> objects,
      std::vector<const FunctionSymbol*> external_functions);
  static bool DumpImage(std::vector<uint8_t> program);

 private:
  const ConstantPool& constant_pool_;
  std::vector<std::string> external_functions_;

  std::optional<NamedBinding::Idx> LinkExternalCallIdx(const std::string& name);

  size_t next_call_idx_{0};
};
