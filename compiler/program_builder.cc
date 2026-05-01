// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

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
  EnterFunctionScope("<<main>>", 0, {}, {});
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
                            .fn = {.argument_count = fn.argc,
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

// static
bool ProgramBuilder::DumpImage(std::vector<uint8_t> program) {
  if (sizeof(vm_prog_header_t) > program.size()) {
    LOG(ERROR) << "Program too small to contain header";
    return false;
  }

  vm_prog_header_t header;
  memcpy(&header, program.data(), sizeof(vm_prog_header_t));

  if (header.magic[0] != 'I' || header.magic[1] != 'N' ||
      header.magic[2] != 'K' || header.magic[3] != '!') {
    LOG(ERROR) << "Invalid magic";
    return false;
  }

  LOG(INFO) << "Program version: " << header.version;
  LOG(INFO) << "Constants: " << header.constant_count;
  LOG(INFO) << "Functions: " << header.function_count;
  LOG(INFO) << "Bytecode Size: " << header.bytecode_size;
  LOG(INFO) << "Debug Size: " << header.debug_size;

  size_t offset = sizeof(vm_prog_header_t);

  size_t parsed_constants = 0;
  size_t parsed_functions = 0;
  std::vector<uint8_t> debug_data;

  struct FunctionInfo {
    uint32_t arg_count;
    uint32_t local_count;
    uint32_t name_offset;
    std::vector<uint8_t> bytecode;
  };
  std::vector<FunctionInfo> functions;

  while (offset + sizeof(vm_section_t) < program.size()) {
    vm_section_t section;
    memcpy(&section, program.data() + offset, sizeof(vm_section_t));
    offset += sizeof(vm_section_t);

    if (offset + section.size > program.size()) {
      LOG(ERROR) << "Section size exceeds program size";
      return false;
    }

    switch (section.type) {
      case vm_section_t::CONST_STR: {
        const char* str =
            reinterpret_cast<const char*>(program.data() + offset);
        LOG(INFO) << "Constant " << parsed_constants++ << ": \""
                  << std::string(str, section.size) << "\"";
        break;
      }

      case vm_section_t::FUNCTION: {
        FunctionInfo fn;
        fn.arg_count = section.fn.argument_count;
        fn.local_count = section.fn.local_count;
        fn.name_offset = section.fn.name_offset;
        fn.bytecode.assign(program.data() + offset,
                           program.data() + offset + section.size);
        functions.push_back(std::move(fn));
        parsed_functions++;
        break;
      }

      case vm_section_t::DEBUG: {
        debug_data.assign(program.data() + offset,
                          program.data() + offset + section.size);
        break;
      }

      default:
        LOG(ERROR) << "Unknown section type: "
                   << static_cast<int>(section.type);
        return false;
    }
    offset += section.size;
  }

  for (size_t i = 0; i < functions.size(); i++) {
    const auto& fn = functions[i];

    const char* name = "<unknown>";

    if (!debug_data.empty() && fn.name_offset < debug_data.size()) {
      name = reinterpret_cast<const char*>(debug_data.data() + fn.name_offset);
    }

    LOG(INFO) << "Function " << i << ": " << name << " (args: " << fn.arg_count
              << ", locals: " << fn.local_count
              << ", bytecode size: " << fn.bytecode.size() << ")";
    DumpByteCode(fn.bytecode);
    printf("\n");
  }

  if (parsed_constants != header.constant_count ||
      parsed_functions != header.function_count) {
    LOG(WARNING) << "counts mismatch";
    LOG(WARNING) << "Constants: " << parsed_constants << " / "
                 << header.constant_count;
    LOG(WARNING) << "Functions: " << parsed_functions << " / "
                 << header.function_count;
  }
  return true;
}