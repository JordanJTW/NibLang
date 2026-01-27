#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "src/types.h"

class Assembler {
 public:
  explicit Assembler() = default;
  ~Assembler() = default;

  Assembler& PushConstRef(uint32_t idx);
  Assembler& PushInt32(int32_t value);
  Assembler& PushFloat(float value);
  Assembler& PushBool(bool value);
  Assembler& Call(uint32_t idx);
  Assembler& Bind(uint32_t idx, uint32_t argc);
  Assembler& PushLocal(uint32_t idx);
  Assembler& Add();
  Assembler& Subtract();
  Assembler& Multiply();
  Assembler& Divide();
  Assembler& And();
  Assembler& Or();
  Assembler& Not();
  Assembler& Increment(uint32_t idx);
  Assembler& Return();
  Assembler& CallNative(uint32_t idx);
  Assembler& CallBuiltIn(uint32_t idx);
  Assembler& StoreLocal(uint32_t idx);
  Assembler& Compare(op_t operation);
  Assembler& JumpIfTrue(uint32_t pc);
  Assembler& JumpIfFalse(uint32_t pc);
  Assembler& Jump(uint32_t pc);
  Assembler& PushTry(uint32_t catch_address, uint32_t finally_address);
  Assembler& PopTry();
  Assembler& Throw();

  Assembler& Label(const std::string& label);
  Assembler& Jump(const std::string& label);
  Assembler& JumpIfTrue(const std::string& label);
  Assembler& JumpIfFalse(const std::string& label);
  Assembler& PushTry(const std::string& catch_label,
                     const std::string& finally_label);

  Assembler& DebugString(const std::string& message);

  struct Metadata {
    uint32_t max_local_index;
  };

  std::vector<uint8_t> Build(Metadata* metadata = nullptr);

 private:
  void PushOpAndArgs(op_t op, std::initializer_list<uint32_t> args);

  // Returns the $pc for a given `label`. If `label` is not yet known then
  // `value_idx` is stored into that label's patch tabel and 0 is returned.
  uint32_t GetLocationForLabel(const std::string& label, uint32_t value_idx);

  std::vector<uint8_t> data_;
  // Labels => the location they map to
  std::map<std::string, uint32_t> label_to_location;
  // Targets for labels that need to be patched during `Assemble()`
  std::map<uint32_t, std::string> patch_locations;
  // The maximum local storage index referenced in the bytecode
  uint32_t max_local_index = 0;
};

void DumpByteCode(const std::vector<uint8_t>& bytecode);
