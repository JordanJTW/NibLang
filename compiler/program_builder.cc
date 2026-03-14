#include "compiler/program_builder.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <stack>
#include <string>
#include <vector>

#include "compiler/logging.h"
#include "src/prog_types.h"
#include "src/vm.h"

ProgramBuilder::ProgramBuilder() {
  EnterFunctionScope("main", 0, {}, {});
}

void ProgramBuilder::EnterFunctionScope(const std::string& name,
                                        size_t call_idx,
                                        std::vector<Symbol> arguments,
                                        std::vector<Symbol> capture_arguments) {
  size_t fn_id = next_func_id_++;
  function_decl_stack_.push(fn_id);

  Function& function = functions_.emplace_back();
  function.name = name;
  function.call_idx = call_idx;
  function.argc = capture_arguments.size() + arguments.size();

  for (const auto& capture : capture_arguments) {
    function.symbol_to_local_idx[capture.idx.value()] = function.next_id++;
  }

  for (const auto& arg : arguments) {
    function.symbol_to_local_idx[arg.idx.value()] = function.next_id++;
  }
}
void ProgramBuilder::ExitFunctionScope() {
  function_decl_stack_.pop();
}

void ProgramBuilder::PushSymbol(Symbol symbol) {
  uint32_t local_idx = GetIdFor(symbol);
  GetCurrentCode().PushLocal(local_idx);
}

void ProgramBuilder::StoreSymbol(Symbol symbol) {
  uint32_t local_idx = GetIdFor(symbol);
  GetCurrentCode().StoreLocal(local_idx);
}

uint32_t ProgramBuilder::GetIdFor(Symbol lookup) {
  auto& current_scope_symbols = GetCurrentScope().symbol_to_local_idx;
  if (auto it = current_scope_symbols.find(lookup.idx.value());
      it != current_scope_symbols.end())
    return it->second;

  uint32_t id = GetCurrentScope().next_id++;
  current_scope_symbols[lookup.idx.value()] = id;
  return id;
}

uint32_t ProgramBuilder::GetIdForConstant(const std::string& value) {
  if (auto it = const_ids_.find(value); it != const_ids_.cend())
    return it->second;

  const_ids_[value] = next_const_id_;
  return next_const_id_++;
}

Assembler& ProgramBuilder::GetCurrentCode() {
  return GetCurrentScope().code;
}

std::vector<uint8_t> ProgramBuilder::GenerateImage() {
  vm_prog_header_t header = {
      .version = 0,
      .function_count = static_cast<uint16_t>(functions_.size()),
      .constant_count = static_cast<uint16_t>(const_ids_.size())};
  memcpy(&header.magic, "INK!", 4);

  size_t offset = sizeof(vm_prog_header_t);
  std::vector<uint8_t> program_image(offset);

  std::sort(functions_.begin(), functions_.end(),
            [](const Function& a, const Function& b) {
              return a.call_idx < b.call_idx;
            });

  std::vector<uint8_t> debug_info;
  std::vector<size_t> debug_offset;
  for (Function fn : functions_) {
    debug_offset.push_back(debug_info.size());

    if (fn.name.empty()) {
      debug_info.push_back('\0');
      continue;
    }

    debug_info.resize(debug_info.size() + fn.name.size() + sizeof(char) /*\0*/);
    memcpy(debug_info.data() + debug_offset.back(), fn.name.data(),
           fn.name.size());
    debug_info[debug_offset.back() + fn.name.size()] = '\0';
  }

  program_image.resize(offset + sizeof(vm_section_t) + debug_info.size());
  vm_section_t section = {.type = vm_section_t::DEBUG,
                          .size = static_cast<uint16_t>(debug_info.size())};

  memcpy(program_image.data() + offset, &section, sizeof(vm_section_t));
  memcpy(program_image.data() + offset + sizeof(vm_section_t),
         debug_info.data(), debug_info.size());
  offset = program_image.size();

  size_t bytecode_size = 0;
  for (Function fn : functions_) {
    std::vector<uint8_t> fn_bytecode = fn.code.Build();
    program_image.resize(offset + sizeof(vm_section_t) + fn_bytecode.size());

    vm_section_t section = {.type = vm_section_t::FUNCTION,
                            .size = static_cast<uint32_t>(fn_bytecode.size()),
                            .as.fn = {.argument_count = fn.argc,
                                      .local_count = fn.next_id,
                                      .name_offset = static_cast<uint16_t>(
                                          debug_offset[fn.call_idx])}};

    memcpy(program_image.data() + offset, &section, sizeof(vm_section_t));
    memcpy(program_image.data() + offset + sizeof(vm_section_t),
           fn_bytecode.data(), fn_bytecode.size());
    bytecode_size += fn_bytecode.size();
    offset = program_image.size();
  }

  header.bytecode_size = bytecode_size;
  header.debug_size = debug_info.size();
  memcpy(program_image.data(), &header, sizeof(vm_prog_header_t));

  std::vector<std::string_view> constants_by_id(const_ids_.size());
  for (const auto& [value, id] : const_ids_) {
    constants_by_id[id] = value;
  }

  for (size_t id = 0; id < constants_by_id.size(); ++id) {
    const auto& value = constants_by_id[id];
    program_image.resize(offset + sizeof(vm_section_t) + value.size());

    vm_section_t section = {.type = vm_section_t::CONST_STR,
                            .size = static_cast<uint16_t>(value.size())};
    memcpy(program_image.data() + offset, &section, sizeof(vm_section_t));
    memcpy(program_image.data() + offset + sizeof(vm_section_t), value.data(),
           value.size());
    offset = program_image.size();
  }

  return program_image;
}