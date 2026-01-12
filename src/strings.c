#include "src/strings.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/vm.h"

static bool is_string(vm_value_t str) {
  return str.type == VALUE_TYPE_STR;
}

static bool is_int32(vm_value_t i32) {
  return i32.type == VALUE_TYPE_INT;
}

vm_value_t vm_strings_substring(vm_value_t* argv, size_t argc, void* vm) {
  assert(argc == 3 && is_string(argv[0]) && is_int32(argv[1]) &&
         is_int32(argv[2]) && "incorrect number of args or arg types");

  char* str;
  size_t len = vm_as_str(&argv[0], &str);

  int32_t start, end;
  if (!vm_as_int32(&argv[1], &start) || !vm_as_int32(&argv[2], &end)) {
    vm_free_ref(argv[0]);  // Free ownership of self
    return (vm_value_t){.type = VALUE_TYPE_NULL};
  }

  if (start >= len || start < 0 || end >= len || end < 0 || start > end) {
    vm_free_ref(argv[0]);  // Free ownership of self
    return (vm_value_t){.type = VALUE_TYPE_NULL};
  }

  vm_value_t result = allocate_str_from_c_with_length(str + start, end - start);
  vm_free_ref(argv[0]);  // Free ownership of self
  return result;
}

vm_value_t vm_strings_get(vm_value_t* argv, size_t argc, void* vm) {
  assert(argc == 2 && is_string(argv[0]) && is_int32(argv[1]) &&
         "incorrect number of args or arg types");

  char* str;
  size_t len = vm_as_str(&argv[0], &str);

  int32_t idx;
  if (!vm_as_int32(&argv[1], &idx) || idx >= len || idx < 0) {
    vm_free_ref(argv[0]);  // Free ownership of self
    return (vm_value_t){.type = VALUE_TYPE_INT, .as.i32 = 0};
  }

  int32_t ch = str[idx];
  vm_free_ref(argv[0]);  // Free ownership of self
  return (vm_value_t){.type = VALUE_TYPE_INT, .as.i32 = ch};
}

vm_value_t vm_strings_starts_with(vm_value_t* argv, size_t argc, void* vm) {
  assert(argc == 3 && is_string(argv[0]) && is_string(argv[1]) &&
         "incorrect number of args or arg types");

  char* str;
  size_t len = vm_as_str(&argv[0], &str);

  char* search_str;
  size_t search_len = vm_as_str(&argv[1], &search_str);

  int32_t idx;

  if (!vm_as_int32(&argv[2], &idx) || idx < 0 || idx >= len ||
      len - idx < search_len) {
    vm_free_ref(argv[0]);
    vm_free_ref(argv[1]);
    return (vm_value_t){.type = VALUE_TYPE_BOOL, .as.boolean = false};
  }

  printf("does '%.*s' start with '%.*s'?\n", (int)len - idx, str + idx,
         (int)search_len, search_str);

  int result = strncmp(str + idx, search_str, search_len);
  vm_free_ref(argv[0]);
  vm_free_ref(argv[1]);
  return (vm_value_t){.type = VALUE_TYPE_BOOL, .as.boolean = (result == 0)};
}