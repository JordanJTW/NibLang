#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "src/types.h"

class Assembler {
 public:
  explicit Assembler() = default;
  ~Assembler() = default;

  Assembler& PushConstRef(uint32_t idx);
  Assembler& PushConst(int32_t value);
  Assembler& Call(uint32_t idx);
  Assembler& PushLocal(uint32_t idx);
  Assembler& Add();
  Assembler& Return();
  Assembler& CallNative(uint32_t idx);
  Assembler& StoreLocal(uint32_t idx);
  Assembler& Compare(op_t operation);
  Assembler& JumpIfFalse(uint32_t pc);
  Assembler& Jump(uint32_t pc);

  Assembler& Label(const std::string& label);
  Assembler& Jump(const std::string& label);
  Assembler& JumpIfFalse(const std::string& label);

  std::vector<uint8_t> Build();

 private:
  void PushOpAndArg32(op_t op, uint32_t arg);

  std::vector<uint8_t> data_;
  // Labels => the location they map to
  std::map<std::string, uint32_t> label_to_location;
  // Targets for labels that need to be patched during `Assemble()`
  std::map<uint32_t, std::string> patch_locations;
};

void DumpByteCode(const std::vector<uint8_t>& bytecode);
