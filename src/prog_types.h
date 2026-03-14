#pragma once

#include <stddef.h>
#include <stdint.h>

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