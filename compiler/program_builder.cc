// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/program_builder.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <optional>
#include <stack>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "compiler/logging.h"
#include "src/prog_types.h"
#include "src/vm.h"

ProgramBuilder::ProgramBuilder(
    const ConstantPool& constant_pool,
    std::span<const std::string_view> external_functions)
    : constant_pool_(constant_pool),
      external_functions_(external_functions.begin(),
                          external_functions.end()) {}

std::optional<NamedBinding::Idx> ProgramBuilder::LinkExternalCallIdx(
    const std::string& name) {
  static const std::unordered_map<std::string, NamedBinding::Idx>
      kBuiltInCallIdx = {
          {"Array_get", VM_BUILTIN_ARRAY_GET},
          {"Array_init", VM_BUILTIN_ARRAY_INIT},
          {"Array_length", VM_BUILTIN_ARRAY_LENGTH},
          {"Array_new", VM_BUILTIN_ARRAY_NEW},
          // Alias for the argument version of the native function.
          {"Array_withSize", VM_BUILTIN_ARRAY_NEW},
          {"Array_push", VM_BUILTIN_ARRAY_PUSH},
          {"Array_set", VM_BUILTIN_ARRAY_SET},
          {"Map_new", VM_BUILTIN_MAP_NEW},
          {"Map_get", VM_BUILTIN_MAP_GET},
          {"Map_set", VM_BUILTIN_MAP_SET},
          {"Math_pow", VM_BUILTIN_MATH_POW},
          {"Promise_fulfill", VM_BUILTIN_PROMISE_FULFILL},
          {"Promise_new", VM_BUILTIN_PROMISE_NEW},
          {"Promise_reject", VM_BUILTIN_PROMISE_REJECT},
          {"Promise_then", VM_BUILTIN_PROMISE_THEN},
          {"String_charAt", VM_BUILTIN_STRINGS_GET},
          {"String_length", VM_BUILTIN_STRING_LENGTH},
          {"String_startsWith", VM_BUILTIN_STRINGS_STARTWITH},
          {"String_substr", VM_BUILTIN_STRINGS_SUBSTRING},
          {"String_valueOf", VM_BUILTIN_STRINGS_VALUEOF},
      };

  // Since some built-in functions (i.e. Array_withSize) overload a single
  // native function, this count can differ from `kBuiltInCallIdx.size()`.
  static constexpr size_t kBuiltInCallCount = 19;

  if (auto it = kBuiltInCallIdx.find(name); it != kBuiltInCallIdx.end())
    return it->second;

  if (auto it = std::find(external_functions_.begin(),
                          external_functions_.end(), name);
      it != external_functions_.end()) {
    // Functions provided to the VM appear directly after built-ins within the
    // function look-up table (calculate the index and convert to VM_BUILTIN).
    return VM_BUILTIN(std::distance(external_functions_.begin(), it) +
                      kBuiltInCallCount);
  }

  return std::nullopt;
}

std::vector<uint8_t> ProgramBuilder::GenerateImage(
    std::vector<ByteCodeGenerator::FunctionObject> objects,
    std::vector<const FunctionSymbol*> external_functions) {
  std::vector<std::string> constants = constant_pool_.GetStrings();

  vm_prog_header_t header = {
      .version = 0,
      .function_count = static_cast<uint16_t>(objects.size()),
      .constant_count = static_cast<uint16_t>(constants.size())};
  memcpy(&header.magic, "INK!", 4);

  size_t offset = sizeof(vm_prog_header_t);
  std::vector<uint8_t> program_image(offset);

  std::sort(objects.begin(), objects.end(),
            [](const ByteCodeGenerator::FunctionObject& a,
               const ByteCodeGenerator::FunctionObject& b) {
              if (a.symbol->declaration.name == "main")
                return true;
              if (b.symbol->declaration.name == "main")
                return false;
              return a.symbol->declaration.name < b.symbol->declaration.name;
            });

  std::vector<uint8_t> debug_info;
  std::vector<size_t> debug_offset;
  for (auto& obj : objects) {
    debug_offset.push_back(debug_info.size());

    const auto& name = obj.symbol->declaration.name;

    if (name.empty()) {
      debug_info.push_back('\0');
      continue;
    }

    debug_info.resize(debug_info.size() + name.size() + sizeof(char) /*\0*/);
    memcpy(debug_info.data() + debug_offset.back(), name.data(), name.size());
    debug_info[debug_offset.back() + name.size()] = '\0';
  }

  program_image.resize(offset + sizeof(vm_section_t) + debug_info.size());
  vm_section_t section = {.type = vm_section_t::DEBUG,
                          .size = static_cast<uint16_t>(debug_info.size())};

  memcpy(program_image.data() + offset, &section, sizeof(vm_section_t));
  memcpy(program_image.data() + offset + sizeof(vm_section_t),
         debug_info.data(), debug_info.size());
  offset = program_image.size();

  std::unordered_map<uint32_t, uint32_t> call_idx_remapping;
  for (size_t i = 0; i < objects.size(); ++i) {
    const FunctionSymbol& symbol = *objects[i].symbol;
    call_idx_remapping[symbol.symbol_id] = next_call_idx_++;
  }

  for (const auto* symbol : external_functions) {
    if (auto call_idx = LinkExternalCallIdx(symbol->GetName()))
      call_idx_remapping[symbol->symbol_id] = *call_idx;
  }

  size_t bytecode_size = 0;
  for (size_t idx = 0; idx < objects.size(); ++idx) {
    const auto& obj = objects[idx];
    std::vector<uint8_t> fn_bytecode =
        obj.bytecode.Build(nullptr, call_idx_remapping);
    program_image.resize(offset + sizeof(vm_section_t) + fn_bytecode.size());

    vm_section_t section = {
        .type = vm_section_t::FUNCTION,
        .size = static_cast<uint32_t>(fn_bytecode.size()),
        .fn = {.argument_count = static_cast<uint16_t>(obj.argument_count),
               .local_count = static_cast<uint16_t>(obj.local_count),
               .name_offset = static_cast<uint16_t>(debug_offset[idx])}};
    memcpy(program_image.data() + offset, &section, sizeof(vm_section_t));
    memcpy(program_image.data() + offset + sizeof(vm_section_t),
           fn_bytecode.data(), fn_bytecode.size());
    bytecode_size += fn_bytecode.size();
    offset = program_image.size();
  }

  header.bytecode_size = bytecode_size;
  header.debug_size = debug_info.size();
  memcpy(program_image.data(), &header, sizeof(vm_prog_header_t));

  for (size_t id = 0; id < constants.size(); ++id) {
    const auto& value = constants[id];
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