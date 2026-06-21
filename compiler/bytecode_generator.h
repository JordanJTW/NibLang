// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "compiler/assembler.h"
#include "compiler/tokenizer.h"
#include "compiler/type_context.h"

class ConstantPool {
 public:
  uint32_t GetIdFor(const std::string& value);

  std::vector<std::string> GetStrings() const;

 private:
  std::unordered_map<std::string, uint32_t> string_constant_to_id_;
  uint32_t next_string_constant_id_ = 0;
};

class ByteCodeGenerator {
 public:
  explicit ByteCodeGenerator(const TypeContext& type_context,
                             const ScopeManager& scope_manager,
                             ConstantPool& constant_pool);

  struct FunctionObject {
    const FunctionSymbol* symbol;
    Assembler bytecode;
    size_t capture_count;
    size_t argument_count;
    size_t local_count;
  };

  FunctionObject Build(const FunctionSymbol& symbol,
                       std::vector<SymbolId>& called_symbols) &&;

 private:
  void PushSymbol(NamedBinding symbol);
  void StoreSymbol(NamedBinding symbol);
  uint32_t GetIdFor(NamedBinding lookup);

  struct LoopContext {
    std::string break_label;
    std::string continue_label;
  };
  void EmitBlock(const Block& block,
                 std::optional<LoopContext> loop_ctx = std::nullopt);

  // How to interpret member/identifier look-up when evaluating an expression.
  enum class AccessMode {
    // Pushes a value onto the stack (i.e. a RHS expression).
    LOAD,
    // Sets up the stack with required values to store it (i.e. LHS expression).
    // An outer expression which enabled STORE is expected to handle the store.
    STORE,
    // Only sets up the target object on the stack but does not try to access
    // it.
    OBJECT_ONLY,
  };

  struct OptionalChainContext {
    std::string null_label;
  };

  void EmitExpression(
      const std::unique_ptr<Expression>& expr,
      std::optional<OptionalChainContext> optional_chain_ctx = std::nullopt,
      AccessMode access_mode = AccessMode::LOAD);

  void EmitCall(const CallExpression& call,
                std::optional<OptionalChainContext> optional_chain_ctx);
  void EmitOp(const Token& op, bool is_string);

  const TypeContext& type_context_;
  const ScopeManager& scope_manager_;
  ConstantPool& constant_pool_;

  Assembler bytecode_;
  size_t next_local_idx_{0};
  size_t next_unique_id_{0};  // Used to generate unique labels

  std::unordered_map<uint32_t, uint32_t> symbol_to_local_idx_;

  std::vector<SymbolId> called_symbols_;
};