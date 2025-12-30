#include "test/assmbler.h"

Assembler& Assembler::PushConstRef(size_t idx) {
  data_.insert(data_.end(), {OP_PUSH_CONST_REF, static_cast<uint8_t>(idx)});
  return *this;
}
Assembler& Assembler::PushConst(int32_t value) {
  data_.insert(data_.end(), {OP_PUSH_CONST, static_cast<uint8_t>(value)});
  return *this;
}
Assembler& Assembler::Call(size_t idx) {
  data_.insert(data_.end(), {OP_CALL, static_cast<uint8_t>(idx)});
  return *this;
}
Assembler& Assembler::PushLocal(size_t idx) {
  data_.insert(data_.end(), {OP_PUSH_LOCAL, static_cast<uint8_t>(idx)});
  return *this;
}
Assembler& Assembler::Add() {
  data_.push_back(OP_ADD);
  return *this;
}
Assembler& Assembler::Return() {
  data_.push_back(OP_RETURN);
  return *this;
}
Assembler& Assembler::CallNative(size_t idx) {
  data_.insert(data_.end(), {OP_CALL_NATIVE, static_cast<uint8_t>(idx)});
  return *this;
}
Assembler& Assembler::StoreLocal(size_t idx) {
  data_.insert(data_.end(), {OP_STORE_LOCAL, static_cast<uint8_t>(idx)});
  return *this;
}
Assembler& Assembler::Compare(op_t operation) {
  data_.push_back(operation);
  return *this;
}
Assembler& Assembler::JumpIfFalse(size_t pc) {
  data_.insert(data_.end(), {OP_JUMP_IF_FALSE, static_cast<uint8_t>(pc)});
  return *this;
}
Assembler& Assembler::Jump(size_t pc) {
  data_.insert(data_.end(), {OP_JUMP, static_cast<uint8_t>(pc)});
  return *this;
}

Assembler& Assembler::Label(const std::string& label) {
  if (auto iter = label_to_location.find(label);
      iter != label_to_location.cend()) {
    fprintf(stderr, "label '%s' already defined at %zu", label.c_str(),
            iter->second);
    std::abort();
  }
  label_to_location[label] = static_cast<uint8_t>(data_.size());
  return *this;
}
Assembler& Assembler::Jump(const std::string& label) {
  if (auto iter = label_to_location.find(label);
      iter != label_to_location.cend()) {
    data_.insert(data_.end(), {OP_JUMP, static_cast<uint8_t>(iter->second)});
  } else {
    patch_locations[data_.size() + 1] = label;
    data_.insert(data_.end(), {OP_JUMP, 0x00 /*dummy address*/});
  }
  return *this;
}
Assembler& Assembler::JumpIfFalse(const std::string& label) {
  if (auto iter = label_to_location.find(label);
      iter != label_to_location.cend()) {
    data_.insert(data_.end(),
                 {OP_JUMP_IF_FALSE, static_cast<uint8_t>(iter->second)});
  } else {
    patch_locations[data_.size() + 1] = label;
    data_.insert(data_.end(), {OP_JUMP_IF_FALSE, 0x00 /*dummy address*/});
  }
  return *this;
}

std::vector<uint8_t> Assembler::Build() {
  for (const auto& [pc, label] : patch_locations) {
    if (auto iter = label_to_location.find(label);
        iter != label_to_location.cend()) {
      data_[pc] = static_cast<uint8_t>(iter->second);
    } else {
      fprintf(stderr, "label '%s' was never defined", label.c_str());
      std::abort();
    }
  }
  return data_;
}
