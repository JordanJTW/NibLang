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

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "compiler/codegen/assembler.h"
#include "compiler/parser/tokenizer.h"
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
  explicit ByteCodeGenerator(const ScopeManager& scope_manager,
                             const TypeRegistry& type_registry,
                             ConstantPool& constant_pool);

  struct FunctionObject {
    const FunctionSymbol* symbol;
    Assembler bytecode;
    size_t argument_count;
    size_t local_count;
  };

  FunctionObject Build(const FunctionSymbol& symbol,
                       std::vector<SymbolId>& called_symbols) &&;

 private:
  struct Slot {
    uint32_t idx, width;
  };

  enum class SuppressFatPointer { YES, NO };
  uint32_t PushSymbol(NamedBinding symbol,
                      SuppressFatPointer suppress_fat_pointer);
  void StoreSymbol(NamedBinding symbol);
  Slot GetSlotFor(NamedBinding lookup);
  uint32_t GetWidthOf(TypeId type_id);

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
    // Sets up the target object on the stack but does not try to access it.
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
                const std::optional<OptionalChainContext>& optional_chain_ctx);
  void EmitOp(const Token& op, bool is_string);

  void PushDispatchIdForTypeId(TypeId type_id);

  const ScopeManager& scope_manager_;
  const TypeRegistry& type_registry_;
  ConstantPool& constant_pool_;

  Assembler bytecode_;
  size_t next_local_idx_{0};
  size_t next_unique_id_{0};  // Used to generate unique labels

  std::unordered_map<uint32_t, Slot> symbol_to_local_slot_;
  std::unordered_map<TypeId, SlotId> template_variable_to_local_slot_;

  std::vector<SymbolId> called_symbols_;
};