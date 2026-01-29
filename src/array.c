#include "src/array.h"

#include <assert.h>
#include <stdlib.h>

#include "src/types.h"
#include "src/vm.h"

vm_value_t vm_array_new(vm_value_t* argv, size_t argc, void* vm) {
  assert(argc == 1 && (argv[0].type == VALUE_TYPE_INT) && "requires size");

  int length = argv[0].as.i32;
  Array* array = calloc(1, sizeof(Array) + length * sizeof(vm_value_t));
  array->len = length;
  array->rc.deleter = &free;
  return (vm_value_t){.type = VALUE_TYPE_ARRAY, .as.array = array};
}

vm_value_t vm_array_get(vm_value_t* argv, size_t argc, void* vm) {
  assert(argc == 2 && (argv[0].type == VALUE_TYPE_ARRAY) &&
         (argv[1].type == VALUE_TYPE_INT) &&
         "incorrect number of args or arg types");

  RC_AUTOFREE vm_value_t this = argv[0];

  Array* array = this.as.array;

  int idx = -1;
  if (!vm_as_int32(&argv[1], &idx) || idx < 0 || idx >= array->len) {
    return vm_throw_exception(vm, allocate_str_from_c("RangeError"));
  }

  vm_value_t result = array->data[idx];
  vm_adopt_ref(result);
  return result;
}

vm_value_t vm_array_set(vm_value_t* argv, size_t argc, void* vm) {
  assert(argc == 3 && (argv[0].type == VALUE_TYPE_ARRAY) &&
         (argv[1].type == VALUE_TYPE_INT) &&
         "incorrect number of args or arg types");

  RC_AUTOFREE vm_value_t this = argv[0];

  Array* array = this.as.array;

  int idx = -1;
  if (!vm_as_int32(&argv[1], &idx) || idx < 0 || idx >= array->len) {
    return vm_throw_exception(vm, allocate_str_from_c("RangeError"));
  }

  vm_free_ref(&array->data[idx]);  // Free whatever is currently there

  array->data[idx] = argv[2];
  return (vm_value_t){.type = VALUE_TYPE_NULL};
}