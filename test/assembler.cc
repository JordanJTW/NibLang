#include "test/assmbler.h"

#include <algorithm>
#include <cassert>

#include "src/vm.h"

Assembler& Assembler::PushConstRef(uint32_t idx) {
  PushOpAndArgs(OP_PUSH_CONST_REF, {idx});
  return *this;
}
Assembler& Assembler::PushInt32(int32_t value) {
  PushOpAndArgs(OP_PUSH_I32, {static_cast<uint32_t>(value)});
  return *this;
}
Assembler& Assembler::PushFloat(float value) {
  data_.push_back(OP_PUSH_F32);
  size_t current_size = data_.size();
  data_.resize(current_size + sizeof(float));
  memcpy(data_.data() + current_size, &value, sizeof(float));
  return *this;
}
Assembler& Assembler::PushBool(bool value) {
  data_.push_back(value ? OP_PUSH_TRUE : OP_PUSH_FALSE);
  return *this;
}
Assembler& Assembler::Call(uint32_t idx) {
  PushOpAndArgs(OP_CALL, {idx});
  return *this;
}
Assembler& Assembler::Bind(uint32_t idx, uint32_t argc) {
  PushOpAndArgs(OP_BIND, {idx, argc});
  return *this;
}
Assembler& Assembler::PushLocal(uint32_t idx) {
  max_local_index = std::max(max_local_index, idx);
  PushOpAndArgs(OP_PUSH_LOCAL, {idx});
  return *this;
}
Assembler& Assembler::Add() {
  data_.push_back(OP_ADD);
  return *this;
}
Assembler& Assembler::Subtract() {
  data_.push_back(OP_SUB);
  return *this;
}
Assembler& Assembler::Multiply() {
  data_.push_back(OP_MUL);
  return *this;
}
Assembler& Assembler::Divide() {
  data_.push_back(OP_DIV);
  return *this;
}
Assembler& Assembler::And() {
  data_.push_back(OP_AND);
  return *this;
}
Assembler& Assembler::Or() {
  data_.push_back(OP_OR);
  return *this;
}
Assembler& Assembler::Not() {
  data_.push_back(OP_NOT);
  return *this;
}
Assembler& Assembler::Increment(uint32_t idx) {
  PushOpAndArgs(OP_INC, {idx});
  return *this;
}
Assembler& Assembler::Return() {
  data_.push_back(OP_RETURN);
  return *this;
}
Assembler& Assembler::CallBuiltIn(uint32_t idx) {
  PushOpAndArgs(OP_CALL, {idx | 0x80000000u});
  return *this;
}
Assembler& Assembler::StoreLocal(uint32_t idx) {
  max_local_index = std::max(max_local_index, idx);
  PushOpAndArgs(OP_STORE_LOCAL, {idx});
  return *this;
}
Assembler& Assembler::Compare(op_t operation) {
  data_.push_back(operation);
  return *this;
}
Assembler& Assembler::JumpIfTrue(uint32_t pc) {
  PushOpAndArgs(OP_JUMP_IF_TRUE, {pc});
  return *this;
}
Assembler& Assembler::JumpIfFalse(uint32_t pc) {
  PushOpAndArgs(OP_JUMP_IF_FALSE, {pc});
  return *this;
}
Assembler& Assembler::Jump(uint32_t pc) {
  PushOpAndArgs(OP_JUMP, {pc});
  return *this;
}

Assembler& Assembler::PushTry(uint32_t catch_address,
                              uint32_t finally_address) {
  PushOpAndArgs(OP_TRY_PUSH, {catch_address, finally_address});
  return *this;
}
Assembler& Assembler::PopTry() {
  data_.push_back(OP_TRY_POP);
  return *this;
}
Assembler& Assembler::Throw() {
  data_.push_back(OP_THROW);
  return *this;
}

Assembler& Assembler::Label(const std::string& label) {
  if (auto iter = label_to_location.find(label);
      iter != label_to_location.cend()) {
    fprintf(stderr, "label '%s' already defined at %u", label.c_str(),
            iter->second);
    std::abort();
  }
  label_to_location[label] = static_cast<uint32_t>(data_.size());
  return *this;
}
Assembler& Assembler::Jump(const std::string& label) {
  uint32_t address = GetLocationForLabel(label, data_.size() + 1);
  PushOpAndArgs(OP_JUMP, {address});
  return *this;
}
Assembler& Assembler::JumpIfTrue(const std::string& label) {
  uint32_t address = GetLocationForLabel(label, data_.size() + 1);
  PushOpAndArgs(OP_JUMP_IF_TRUE, {address});
  return *this;
}
Assembler& Assembler::JumpIfFalse(const std::string& label) {
  uint32_t address = GetLocationForLabel(label, data_.size() + 1);
  PushOpAndArgs(OP_JUMP_IF_FALSE, {address});
  return *this;
}

Assembler& Assembler::PushTry(const std::string& catch_label,
                              const std::string& finally_label) {
  uint32_t catch_addr = GetLocationForLabel(catch_label, data_.size() + 1);
  uint32_t finally_addr = GetLocationForLabel(finally_label, data_.size() + 5);
  PushOpAndArgs(OP_TRY_PUSH, {catch_addr, finally_addr});
  return *this;
}

uint32_t Assembler::GetLocationForLabel(const std::string& label,
                                        uint32_t value_idx) {
  if (auto iter = label_to_location.find(label);
      iter != label_to_location.cend()) {
    return iter->second;
  }

  patch_locations[value_idx] = label;
  return 0;
}

Assembler& Assembler::DebugString(const std::string& message) {
  assert(message.size() < 256 && "message can only be 255 chars");
  data_.push_back(OP_DEBUG);
  data_.push_back(static_cast<uint8_t>(message.size()));
  for (char c : message) {
    data_.push_back(static_cast<uint8_t>(c));
  }
  return *this;
}

std::vector<uint8_t> Assembler::Build(Metadata* metadata) {
  for (const auto& [pc, label] : patch_locations) {
    if (auto iter = label_to_location.find(label);
        iter != label_to_location.cend()) {
      uint32_t address = iter->second;
      data_[pc] = address & 0xFF;
      data_[pc + 1] = (address >> 8) & 0xFF;
      data_[pc + 2] = (address >> 16) & 0xFF;
      data_[pc + 3] = (address >> 24) & 0xFF;
    } else {
      fprintf(stderr, "label '%s' was never defined", label.c_str());
      std::abort();
    }
  }
  if (metadata != nullptr) {
    metadata->max_local_index = max_local_index;
  }
  return data_;
}

void Assembler::PushOpAndArgs(op_t op, std::initializer_list<uint32_t> args) {
  // Push opcode
  data_.push_back(op);

  // Push each 32-bit argument (little-endian)
  for (uint32_t arg : args) {
    data_.push_back(uint8_t(arg & 0xFF));
    data_.push_back(uint8_t((arg >> 8) & 0xFF));
    data_.push_back(uint8_t((arg >> 16) & 0xFF));
    data_.push_back(uint8_t((arg >> 24) & 0xFF));
  }
}

std::string GetOpName(op_t op) {
  switch (op) {
#define CASE_OP_NAME($op) \
  case $op:               \
    return #$op

    CASE_OP_NAME(OP_PUSH_CONST_REF);
    CASE_OP_NAME(OP_PUSH_I32);
    CASE_OP_NAME(OP_PUSH_F32);
    CASE_OP_NAME(OP_PUSH_LOCAL);
    CASE_OP_NAME(OP_PUSH_TRUE);
    CASE_OP_NAME(OP_PUSH_FALSE);
    CASE_OP_NAME(OP_STORE_LOCAL);
    CASE_OP_NAME(OP_CALL);
    CASE_OP_NAME(OP_RETURN);
    CASE_OP_NAME(OP_BIND);
    CASE_OP_NAME(OP_ADD);
    CASE_OP_NAME(OP_SUB);
    CASE_OP_NAME(OP_MUL);
    CASE_OP_NAME(OP_DIV);
    CASE_OP_NAME(OP_AND);
    CASE_OP_NAME(OP_OR);
    CASE_OP_NAME(OP_NOT);
    CASE_OP_NAME(OP_INC);
    CASE_OP_NAME(OP_LESS_THAN);
    CASE_OP_NAME(OP_LESS_OR_EQ);
    CASE_OP_NAME(OP_EQUAL);
    CASE_OP_NAME(OP_GREAT_OR_EQ);
    CASE_OP_NAME(OP_GREATER_THAN);
    CASE_OP_NAME(OP_JUMP_IF_TRUE);
    CASE_OP_NAME(OP_JUMP_IF_FALSE);
    CASE_OP_NAME(OP_JUMP);
    CASE_OP_NAME(OP_TRY_PUSH);
    CASE_OP_NAME(OP_TRY_POP);
    CASE_OP_NAME(OP_THROW);
    CASE_OP_NAME(OP_DEBUG);
#undef CASE_OP_NAME
  }
}

void DumpByteCode(const std::vector<uint8_t>& bytecode) {
  for (size_t pc = 0; pc < bytecode.size();) {
    op_t op = static_cast<op_t>(bytecode[pc]);
    switch (op) {
      case OP_PUSH_CONST_REF:
      case OP_PUSH_I32:
      case OP_CALL:
      case OP_PUSH_LOCAL:
      case OP_STORE_LOCAL: {
        uint32_t arg = bytecode[pc + 1] | (bytecode[pc + 2] << 8) |
                       (bytecode[pc + 3] << 16) | (bytecode[pc + 4] << 24);
        printf(
            "0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x,"
            " // %04zx: %s %d \n",
            bytecode[pc], bytecode[pc + 1], bytecode[pc + 2], bytecode[pc + 3],
            bytecode[pc + 4], pc, GetOpName(op).c_str(), arg);
        pc += 5;
        break;
      }
      case OP_PUSH_F32: {
        float arg = 0;
        memcpy(&arg, bytecode.data() + pc + 1, 4);
        printf(
            "0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x,"
            " // %04zx: %s %f \n",
            bytecode[pc], bytecode[pc + 1], bytecode[pc + 2], bytecode[pc + 3],
            bytecode[pc + 4], pc, GetOpName(op).c_str(), arg);
        pc += 5;
        break;
      }
      case OP_JUMP_IF_TRUE:
      case OP_JUMP_IF_FALSE:
      case OP_JUMP: {
        uint32_t arg = bytecode[pc + 1] | (bytecode[pc + 2] << 8) |
                       (bytecode[pc + 3] << 16) | (bytecode[pc + 4] << 24);
        printf(
            "0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x,"
            " // %04zx: %s 0x%x \n",
            bytecode[pc], bytecode[pc + 1], bytecode[pc + 2], bytecode[pc + 3],
            bytecode[pc + 4], pc, GetOpName(op).c_str(), arg);
        pc += 5;
        break;
      }
      case OP_BIND: {
        uint32_t idx = bytecode[pc + 1] | (bytecode[pc + 2] << 8) |
                       (bytecode[pc + 3] << 16) | (bytecode[pc + 4] << 24);
        uint32_t argc = bytecode[pc + 5] | (bytecode[pc + 6] << 8) |
                        (bytecode[pc + 7] << 16) | (bytecode[pc + 8] << 24);
        printf(
            "0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x,"
            " // %04zx: %s idx: %d argc: %d\n"
            "      0x%02x 0x%02x 0x%02x 0x%02x,",
            bytecode[pc], bytecode[pc + 1], bytecode[pc + 2], bytecode[pc + 3],
            bytecode[pc + 4], pc, GetOpName(op).c_str(), idx, argc,
            bytecode[pc + 5], bytecode[pc + 6], bytecode[pc + 7],
            bytecode[pc + 8]);
        pc += 9;
        break;
      }
      case OP_TRY_PUSH: {
        uint32_t catch_addr = bytecode[pc + 1] | (bytecode[pc + 2] << 8) |
                              (bytecode[pc + 3] << 16) |
                              (bytecode[pc + 4] << 24);
        uint32_t finally_addr = bytecode[pc + 5] | (bytecode[pc + 6] << 8) |
                                (bytecode[pc + 7] << 16) |
                                (bytecode[pc + 8] << 24);
        printf(
            "0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x,"
            " // %04zx: %s catch: 0x%x finally: 0x%x\n"
            "      0x%02x 0x%02x 0x%02x 0x%02x,",
            bytecode[pc], bytecode[pc + 1], bytecode[pc + 2], bytecode[pc + 3],
            bytecode[pc + 4], pc, GetOpName(op).c_str(), catch_addr,
            finally_addr, bytecode[pc + 5], bytecode[pc + 6], bytecode[pc + 7],
            bytecode[pc + 8]);
        pc += 9;
        break;
      }
      case OP_DEBUG: {
        uint8_t strlen = bytecode[pc + 1];
        printf("strlen: %d\n", strlen);
        fprintf(stderr, "  %04zx => %.*s\n", pc, (int)strlen,
                (char*)(bytecode.data() + pc + 2));
        pc += 2 + strlen;
        break;
      }
      case OP_ADD:
      case OP_RETURN:
      case OP_LESS_THAN:
      case OP_LESS_OR_EQ:
      case OP_EQUAL:
      case OP_GREAT_OR_EQ:
      case OP_GREATER_THAN:
      case OP_SUB:
      case OP_MUL:
      case OP_DIV:
      case OP_AND:
      case OP_OR:
      case OP_NOT:
      case OP_INC:
      case OP_PUSH_TRUE:
      case OP_PUSH_FALSE:
      case OP_TRY_POP:
      case OP_THROW: {
        printf("0x%02x, // %04zx: %s\n", bytecode[pc], pc,
               GetOpName(op).c_str());
        pc += 1;
        break;
      }
    }
  }
}