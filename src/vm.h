#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "src/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* vm_t;

// Represents an interpreted function with `data_len` bytes of bytecode in
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
// Ownership of the `argv` arguments is passed to `func` and must be explicitly
// freed if not retained (see `vm_free_ref()`).
typedef struct {
  size_t arg_count;
  vm_value_t (*func)(vm_value_t* argv, size_t argc, void* userdata);
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

// Runs `functions[0]` until a OP_RETURN is encountered.
void vm_run(vm_t vm);

// Provides `value` as an i32 in `out` and returns true. If `value` is NULL or
// is not an i32 then returns false and `out` is untouched.
bool vm_as_int32(const vm_value_t* value, int32_t* out);

// Provides `value` as a C string in `out` and returns length. If `value` is
// NULL or is not s string then returns 0 and `out` is untouched.
size_t vm_as_str(const vm_value_t* value, char** out);

// Allocates a new heap allocated string and returns its ref in `vm_value_t`.
vm_value_t allocate_str_from_c(const char* str);
vm_value_t allocate_str_from_c_with_length(const char* str, size_t len);

// If `value` is a reference type (i.e. heap allocated), this function will
// free ownership of the reference. If `value` is not a reference this is no-op.
void vm_free_ref(vm_value_t value);

typedef struct vm_function_t {
  enum { NATIVE, FUNC_IDX } type;
  union {
    vm_native_func_t native;
    size_t func_idx;
  } is;
} Function;

vm_value_t vm_call_function(vm_t vm,
                            Function* fn,
                            size_t argc,
                            vm_value_t* argv);

#ifdef __cplusplus
}  // extern "C"
#endif