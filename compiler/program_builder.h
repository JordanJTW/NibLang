// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <stack>
#include <string>
#include <vector>

#include "compiler/assembler.h"
#include "compiler/types.h"

class ProgramBuilder {
 public:
  explicit ProgramBuilder();

  void EnterFunctionScope(const std::string& name,
                          size_t call_idx,
                          std::vector<Symbol> arguments,
                          std::vector<Symbol> capture_arguments);
  void ExitFunctionScope();

  uint32_t GetIdForConstant(const std::string& value);
  void PushSymbol(Symbol symbol);
  void StoreSymbol(Symbol symbol);
  Assembler& GetCurrentCode();

  std::vector<uint8_t> GenerateImage();
  static bool DumpImage(std::vector<uint8_t> program);

 private:
  uint32_t GetIdFor(Symbol lookup);

  struct Function {
    std::string name;
    size_t call_idx;
    std::unordered_map<Symbol::Idx, size_t> symbol_to_local_idx;
    uint16_t next_id{0};
    uint16_t argc{0};
    Assembler code;
  };
  std::vector<Function> functions_;
  std::map<std::string, int> func_ids_;
  int next_func_id_{0};
  std::stack<int> function_decl_stack_;

  std::map<std::string, int> const_ids_;
  int next_const_id_{0};

  Function& GetCurrentScope() {
    int fn_id = function_decl_stack_.top();
    return functions_.at(fn_id);
  }
};
