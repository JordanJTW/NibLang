// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <optional>
#include <string>

#include "compiler/program_builder.h"
#include "compiler/tokenizer.h"
#include "compiler/type_context.h"

class ByteCodeGenerator {
 public:
  explicit ByteCodeGenerator(ProgramBuilder& program_builder,
                             const TypeContext& type_context);

  void EmitBlock(const Block& block);

 private:
  void EmitOp(const Token& op, bool is_string);

  struct LoopContext {
    std::string break_label;
    std::string continue_label;
  };
  void EmitBlock(const Block& block, std::optional<LoopContext> loop_ctx);
  void EmitFunctionBlock(
      const FunctionDeclaration& fn,
      std::optional<std::string> override_name = std::nullopt);

  struct OptionalChainContext {
    std::string null_label;
  };

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

  void EmitExpression(
      const std::unique_ptr<Expression>& expr,
      std::optional<OptionalChainContext> optional_chain_ctx = std::nullopt,
      AccessMode access_mode = AccessMode::LOAD);
  void EmitCall(const CallExpression& call,
                std::optional<OptionalChainContext> optional_chain_ctx);

  ProgramBuilder& builder_;
  const TypeContext& type_context_;
  size_t next_unique_id_{0};  // Used to generate unique labels
};