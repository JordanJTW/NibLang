#include "src/strings.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "src/vm.h"

static bool is_string(vm_value_t str) {
  return str.type == VALUE_TYPE_STR;
}

static bool is_int32(vm_value_t i32) {
  return i32.type == VALUE_TYPE_INT;
}

vm_value_t vm_strings_substring(vm_value_t* argv, size_t argc, void*) {
  assert(argc == 3 && is_string(argv[0]) && is_int32(argv[1]) &&
         is_int32(argv[2]) && "incorrect number of args or arg types");

  char* str;
  size_t len = vm_as_str(&argv[0], &str);

  int32_t start, end;
  if (!vm_as_int32(&argv[1], &start) || !vm_as_int32(&argv[2], &end)) {
    vm_free_ref(argv[0]);  // Free ownership of self
    return (vm_value_t){.type = VALUE_TYPE_NULL};
  }

  if (start > len || start < 0 || end > len || end < 0 || start > end) {
    vm_free_ref(argv[0]);  // Free ownership of self
    return (vm_value_t){.type = VALUE_TYPE_NULL};
  }

  vm_value_t result = allocate_str_from_c_with_length(str + start, end - start);
  vm_free_ref(argv[0]);  // Free ownership of self
  return result;
}