#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* vm_t;
typedef struct vm_string_t String;

typedef struct vm_value {
  enum {
    VALUE_TYPE_INT,
    VALUE_TYPE_STR,
  } type;
  union {
    int32_t i32;
    String* str;
  } as;
} vm_value_t;

// A fixed sized (`capacity`) stack of `vm_value_t` for the VM.
typedef struct {
  vm_value_t* values;
  size_t sp;
  size_t capacity;
} vm_stack_t;

// Represents an interpretted function with `data_len` bytes of bytecode in
// `data`. `arg_count` is the number of arguments this function takes and
// `local_count` is count of ALL local variables (i.e. `local_count` ⊇
// `arg_count`). `name` is used for debugging.
typedef struct {
  const uint8_t* data;
  size_t data_len;
  size_t arg_count;
  size_t local_count;
  const char* name;
} vm_func_t;

// Represents a native function `func`. `func` is invoked with a pointer to the
// head of the current stack in `argv`, `arg_count` in `argc`, and `userdata`.
typedef struct {
  size_t arg_count;
  void (*func)(vm_value_t* argv, size_t argc, void* userdata);
  void* userdata;
  const char* name;
} vm_native_func_t;

// Allocates a new VM.
vm_t new_vm(vm_value_t* constants,
            size_t constants_count,
            vm_func_t* functions,
            size_t functions_count,
            vm_native_func_t* native_functions,
            size_t native_functions_count);
void free_vm(vm_t vm);

// Runs `functions[0]` until a RETURN is encountered.
void vm_run(vm_t vm);

// Provides `value` as an i32 in `out` and returns true. If `value` is NULL or
// is not an i32 then returns false and `out` is untouched.
bool vm_as_int32(vm_value_t* value, int32_t* out);

// Provides `value` as a C string in `out` and returns length. If `value` is
// NULL or is not s string then returns 0 and `out` is untouched.
size_t vm_as_str(vm_value_t* value, char** out);

vm_value_t allocate_str_from_c(const char* str);

#ifdef __cplusplus
}  // extern "C"
#endif