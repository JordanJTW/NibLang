// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "src/strings.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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

  RC_AUTOFREE vm_value_t this = argv[0];

  char* cstr;
  size_t len = vm_as_str(&this, &cstr);

  int32_t start, end;
  if (!vm_as_int32(&argv[1], &start) || !vm_as_int32(&argv[2], &end)) {
    return vm_throw_exception(vm, allocate_str_from_c("TypeError"));
  }

  if (start < 0 || end < 0 || end > len || start > end) {
    return vm_throw_exception(vm, allocate_str_from_c("RangeError"));
  }

  return allocate_str_from_c_with_len(cstr + start, end - start);
}

vm_value_t vm_strings_get(vm_value_t* argv, size_t argc, void* vm) {
  assert(argc == 2 && is_string(argv[0]) && is_int32(argv[1]) &&
         "incorrect number of args or arg types");

  RC_AUTOFREE vm_value_t this = argv[0];

  char* cstr;
  size_t len = vm_as_str(&this, &cstr);

  int32_t idx;
  if (!vm_as_int32(&argv[1], &idx) || idx >= len || idx < 0) {
    return vm_throw_exception(vm, allocate_str_from_c("RangeError"));
  }

  return (vm_value_t){.type = VALUE_TYPE_INT, .as.i32 = cstr[idx]};
}

vm_value_t vm_strings_starts_with(vm_value_t* argv, size_t argc, void* vm) {
  assert(argc == 3 && is_string(argv[0]) && is_string(argv[1]) &&
         "incorrect number of args or arg types");

  RC_AUTOFREE vm_value_t this = argv[0];
  RC_AUTOFREE vm_value_t target = argv[1];

  char* cstr;
  size_t len = vm_as_str(&this, &cstr);

  char* search_str;
  size_t search_len = vm_as_str(&target, &search_str);

  int32_t idx;
  if (!vm_as_int32(&argv[2], &idx)) {
    return vm_throw_exception(vm, allocate_str_from_c("TypeError"));
  }

  int result = strncmp(cstr + idx, search_str, search_len);
  return (vm_value_t){.type = VALUE_TYPE_BOOL, .as.boolean = (result == 0)};
}

vm_value_t vm_string_length(vm_value_t* argv, size_t argc, void* vm) {
  assert(argc == 1 && (argv[0].type == VALUE_TYPE_STR) &&
         "incorrect number of args or arg types");

  RC_AUTOFREE vm_value_t this = argv[0];

  return (vm_value_t){.type = VALUE_TYPE_INT, .as.i32 = this.as.str->len};
}

vm_value_t vm_string_valueof(vm_value_t* argv, size_t argc, void* vm) {
  assert(argc == 1);

  vm_value_t number = argv[0];

  char buffer[12];
  switch (number.type) {
    case VALUE_TYPE_NULL:
      return allocate_str_from_c("Nil");
    case VALUE_TYPE_BOOL:
      return allocate_str_from_c(number.as.boolean ? "true" : "false");
    case VALUE_TYPE_FLOAT:
      snprintf(buffer, sizeof(buffer), "%.1f", number.as.f32);
      return allocate_str_from_c(buffer);
    case VALUE_TYPE_INT:
      snprintf(buffer, sizeof(buffer), "%d", number.as.i32);
      return allocate_str_from_c(buffer);
    case VALUE_TYPE_STR:
      return number;
    default:
      return allocate_str_from_c("<non-primitive value>");
  }
}