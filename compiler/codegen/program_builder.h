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

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "assembler.h"
#include "bytecode_generator.h"
#include "compiler/types.h"

class ProgramBuilder {
 public:
  explicit ProgramBuilder(
      const ConstantPool& constant_pool,
      std::span<const std::string_view> external_functions = {});

  std::vector<uint8_t> GenerateImage(
      std::vector<ByteCodeGenerator::FunctionObject> objects,
      std::vector<const FunctionSymbol*> external_functions,
      std::vector<const FunctionSymbol*> virtual_functions = {});
  static bool DumpImage(std::vector<uint8_t> program);

 private:
  const ConstantPool& constant_pool_;
  std::vector<std::string> external_functions_;

  std::optional<NamedBinding::Idx> LinkExternalCallIdx(const std::string& name);

  size_t next_call_idx_{0};
};
