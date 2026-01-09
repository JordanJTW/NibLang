#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "src/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VM_BUILTIN_SELECT_BITMASK 0x80000000u
#define VM_BUILTIN($idx) $idx | VM_BUILTIN_SELECT_BITMASK

typedef struct vm_function_t {
  enum { VM_BYTECODE, VM_NATIVE_FUNC } type;
  size_t argument_count;
  const char* name;
  union {
    // Represents an interpreted function with `data_len` bytes of bytecode in
    // `data`. `local_count` is the number of local variables to allocate in
    // frame (`local_count` ⊇ `argument_count`).
    struct vm_bytecode_t {
      const uint8_t* data;
      size_t data_len;
      size_t local_count;
    } bytecode;

    // Represents a native function `fn`. `fn` is invoked with a pointer to
    // the head of the current stack in `argv`, `argument_count` in `argc`, and
    // `userdata`. Ownership of the `argv` arguments is passed to `func` and
    // must be explicitly freed if not retained (see `vm_free_ref()`).
    struct vm_native_t {
      vm_value_t (*fn)(vm_value_t* argv, size_t argc, void* userdata);
      void* userdata;
    } native;
  } as;
} vm_function_t;

typedef struct vm_closure_t {
  ref_count_t ref_count;
  vm_function_t* fn;
  size_t bound_argc;
  vm_value_t argument_storage[];
} Closure;

// Allocates a new VM.
vm_t* new_vm(vm_value_t* constants,
             size_t constants_count,
             vm_function_t* functions,
             size_t functions_count);
void free_vm(vm_t* vm);

// Runs the function at `entery_point_idx` until OP_RETURN is encountered.
void vm_run(vm_t* vm, size_t entry_point_idx);

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

// If `value` is a reference type (i.e. heap allocated), this function will
// "adopt" the `value` (increment the ref count) so that ownerhip is retained.
void vm_adopt_ref(vm_value_t value);

vm_value_t vm_call_function(vm_t* vm,
                            Closure* fn,
                            vm_value_t* argv,
                            size_t argc);

vm_value_t bind_to_function(vm_t* vm,
                            size_t idx,
                            vm_value_t* argv,
                            size_t argc);

vm_job_queue_t* vm_get_job_queue(vm_t* vm);

#ifdef __cplusplus
}  // extern "C"
#endif