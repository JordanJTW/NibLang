#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string>
#include <vector>

#include "src/op.h"

class Assembler {
 public:
  explicit Assembler() = default;
  ~Assembler() = default;

  Assembler& PushConstRef(size_t idx);
  Assembler& PushConst(int32_t value);
  Assembler& Call(size_t idx);
  Assembler& PushLocal(size_t idx);
  Assembler& Add();
  Assembler& Return();
  Assembler& CallNative(size_t idx);
  Assembler& StoreLocal(size_t idx);
  Assembler& Compare(op_t operation);
  Assembler& JumpIfFalse(size_t pc);
  Assembler& Jump(size_t pc);

  Assembler& Label(const std::string& label);
  Assembler& Jump(const std::string& label);
  Assembler& JumpIfFalse(const std::string& label);

  std::vector<uint8_t> Build();

 private:
  std::vector<uint8_t> data_;
  // Labels => the location they map to
  std::map<std::string, size_t> label_to_location;
  // Targets for labels that need to be patched during `Assemble()`
  std::map<size_t, std::string> patch_locations;
};