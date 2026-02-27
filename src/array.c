#include "src/array.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#include "src/types.h"
#include "src/vm.h"

void vm_array_destroy(void* ptr) {
  Array* array = ptr;

  for (size_t i = 0; i < array->len; i++) {
    vm_free_ref(&array->data[i]);
  }

  free(array->data);
  free(array);
}

Array* allocate_array(int length) {
  Array* const array = calloc(1, sizeof(Array));
  array->capacity = length;
  array->len = length;
  array->data = calloc(length, sizeof(vm_value_t));
  array->rc.deleter = &vm_array_destroy;
  return array;
}

vm_value_t vm_array_new(vm_value_t* argv, size_t argc, void* vm) {
  int length = argc == 1 ? argv[0].as.i32 : 0;
  return (vm_value_t){.type = VALUE_TYPE_ARRAY,
                      .as.array = allocate_array(length)};
}

vm_value_t vm_array_init(vm_value_t* argv, size_t argc, void* vm) {
  int length = argc >= 1 ? argv[0].as.i32 : 0;
  assert((argc == length + 1) && "length and argc must match");
  Array* array = allocate_array(length);

  for (int i = 1; i <= length; ++i) {
    array->data[i - 1] = argv[i];
  }
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

vm_value_t vm_array_push(vm_value_t* argv, size_t argc, void* vm) {
  assert(argc == 2 && (argv[0].type == VALUE_TYPE_ARRAY) &&
         "incorrect number of args or arg types");

  RC_AUTOFREE vm_value_t this = argv[0];

  Array* array = this.as.array;

  if (array->len == array->capacity) {
    static const size_t kInitialCapacity = 2;

    size_t new_capacity =
        array->capacity ? array->capacity * 2 : kInitialCapacity;

    void* tmp = realloc(array->data, new_capacity * sizeof(vm_value_t));
    if (!tmp) {
      return vm_throw_exception(vm, allocate_str_from_c("Out of memory"));
    }

    array->capacity = new_capacity;
    array->data = tmp;
  }

  array->data[array->len++] = argv[1];
  return (vm_value_t){.type = VALUE_TYPE_NULL};
}