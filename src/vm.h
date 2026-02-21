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

enum {
  VM_BUILTIN_PROMISE_NEW = VM_BUILTIN(0),
  VM_BUILTIN_PROMISE_FULFILL = VM_BUILTIN(1),
  VM_BUILTIN_PROMISE_REJECT = VM_BUILTIN(2),
  VM_BUILTIN_PROMISE_THEN = VM_BUILTIN(3),
  VM_BUILTIN_STRINGS_SUBSTRING = VM_BUILTIN(4),
  VM_BUILTIN_MAP_NEW = VM_BUILTIN(5),
  VM_BUILTIN_STRINGS_GET = VM_BUILTIN(6),
  VM_BUILTIN_MAP_SET = VM_BUILTIN(7),
  VM_BUILTIN_STRINGS_STARTWITH = VM_BUILTIN(8),
  VM_BUILTIN_MATH_POW = VM_BUILTIN(9),
  VM_BUILTIN_ARRAY_NEW = VM_BUILTIN(10),
  VM_BUILTIN_ARRAY_GET = VM_BUILTIN(11),
  VM_BUILTIN_ARRAY_SET = VM_BUILTIN(12),
  VM_BUILTIN_LOG = VM_BUILTIN(13),
  VM_BUILTIN_STRING_LENGTH = VM_BUILTIN(14),
  VM_BUILTIN_ARRAY_INIT = VM_BUILTIN(15),
};

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
vm_t* init_vm(uint8_t* program, size_t program_size);
vm_t* new_vm(vm_value_t* constants,
             size_t constants_count,
             vm_function_t* functions,
             size_t functions_count);
void free_vm(vm_t* vm);

// Runs the function at `entery_point_idx` until OP_RETURN is encountered.
vm_value_t vm_run(vm_t* vm, size_t entry_point_idx, bool pop_return);

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
void vm_free_ref(vm_value_t* value);

// Uses GCC attribute to automatically call `vm_free_ref` on stack unwind.
// Example Usage:
//   RC_AUTOFREE vm_value_t this = argv[0];
#define RC_AUTOFREE __attribute__((cleanup(vm_free_ref)))

// If `value` is a reference type (i.e. heap allocated), this function will
// "adopt" the `value` (increment the ref count) so that ownerhip is retained.
void vm_adopt_ref(vm_value_t value);

// Returns any currently unhandled exception (and clears it from needing to be
// handled). `exception` will NOT be touched if function returns false.
//
// NOTE: Calling this function will clear the active exception as handled.
bool vm_get_exception(vm_t* vm, vm_value_t* exception);

// Throws `exception` to be handled on the next interpeter iteration. Returns
// a VALUE_TYPE_NULL as a convenience. This is expected be called from native.
vm_value_t vm_throw_exception(vm_t* vm, vm_value_t exception);

// Returns the async job queue associated with `vm`.
vm_job_queue_t* vm_get_job_queue(vm_t* vm);

vm_value_t vm_call_function(vm_t* vm,
                            Closure* fn,
                            vm_value_t* argv,
                            size_t argc);

vm_value_t bind_to_function(vm_t* vm,
                            size_t idx,
                            vm_value_t* argv,
                            size_t argc);

#ifdef __cplusplus
}  // extern "C"
#endif