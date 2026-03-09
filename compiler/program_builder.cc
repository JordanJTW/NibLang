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
#include "src/vm.h"

ProgramBuilder::ProgramBuilder() {
  func_ids_["String_charAt"] = VM_BUILTIN_STRINGS_GET;
  func_ids_["String_substr"] = VM_BUILTIN_STRINGS_SUBSTRING;
  func_ids_["String_startsWith"] = VM_BUILTIN_STRINGS_STARTWITH;
  func_ids_["String_length"] = VM_BUILTIN_STRING_LENGTH;
  func_ids_["Map_new"] = VM_BUILTIN_MAP_NEW;
  func_ids_["Map_set"] = VM_BUILTIN_MAP_SET;
  func_ids_["Array_new"] = VM_BUILTIN_ARRAY_NEW;
  func_ids_["Array_init"] = VM_BUILTIN_ARRAY_INIT;
  func_ids_["Array_get"] = VM_BUILTIN_ARRAY_GET;
  func_ids_["Array_set"] = VM_BUILTIN_ARRAY_SET;

  EnterFunctionScope("main", 0, {}, {});
}

void ProgramBuilder::EnterFunctionScope(
    const std::string& name,
    size_t call_idx,
    std::vector<std::pair<std::string, ParsedType>> arguments,
    std::vector<std::string> captures) {
  size_t fn_id = next_func_id_++;
  function_decl_stack_.push(fn_id);

  Function& function = functions_.emplace_back();
  function.name = name;
  function.call_idx = call_idx;
  for (const auto& capture : captures) {
    function.var_to_id[capture] = function.next_id++;
    ++function.argc;
  }

  for (const auto& arg : arguments) {
    function.var_to_id[arg.first] = function.next_id++;
    ++function.argc;
  }
}
void ProgramBuilder::ExitFunctionScope() {
  function_decl_stack_.pop();
}

uint32_t ProgramBuilder::GetIdForConstant(const std::string& value) {
  if (auto it = const_ids_.find(value); it != const_ids_.cend())
    return it->second;

  const_ids_[value] = next_const_id_;
  return next_const_id_++;
}

std::optional<uint32_t> ProgramBuilder::GetIdFor(const std::string& name,
                                                 CreateIfMissing create) {
  Function& fn = GetCurrentScope();
  if (auto it = fn.var_to_id.find(name); it != fn.var_to_id.cend())
    return it->second;

  if (create == CreateIfMissing::No)
    return std::nullopt;

  fn.var_to_id[name] = fn.next_id;
  return fn.next_id++;
}

Assembler& ProgramBuilder::GetCurrentCode() {
  return GetCurrentScope().code;
}

void ProgramBuilder::CallFunction(const std::string& name, size_t argc) {
  auto it = func_ids_.find(name);
  if (it == func_ids_.end()) {
    functions_.at(function_decl_stack_.top())
        .unresolved_calls[GetCurrentCode().offset()] = name;
    GetCurrentCode().Call(UINT32_MAX, argc);
    return;
  }

  GetCurrentCode().Call(it->second, argc);
}

#pragma pack(push, 1)
typedef struct vm_prog_header_t {
  uint32_t version;
  char magic[4];
  uint16_t function_count;
  uint16_t constant_count;
  uint32_t bytecode_size;
  uint32_t debug_size;
} vm_prog_header_t;

typedef struct vm_prog_function_t {
  uint16_t argument_count;
  uint16_t local_count;
  uint16_t name_offset;
  uint8_t bytecode[];
} vm_prog_function_t;

typedef struct vm_section_t {
  enum : uint8_t { CONST_STR, FUNCTION, DEBUG } type;
  uint32_t size;
  union {
    vm_prog_function_t fn;
    char str[];
  } as;
} vm_section_t;
#pragma pack(pop)

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
    // std::cout << "Bytecode for function '" << fn.name << "':\n";
    // DumpByteCode(fn_bytecode);

    for (const auto& [offset, fn] : fn.unresolved_calls) {
      assert(fn_bytecode[offset] == OP_CALL && "expceted OP_CALL");

      auto it = func_ids_.find(fn);
      assert(it != func_ids_.end() && "unknown function");
      fn_bytecode[offset + 1] = uint8_t(it->second & 0xFF);
      fn_bytecode[offset + 2] = uint8_t((it->second >> 8) & 0xFF);
      fn_bytecode[offset + 3] = uint8_t((it->second >> 16) & 0xFF);
      fn_bytecode[offset + 4] = uint8_t((it->second >> 24) & 0xFF);
    }

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