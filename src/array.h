#pragma once

#include <stddef.h>

#include "src/types.h"

vm_value_t allocate_array(vm_t* vm, int length);

vm_value_t vm_array_new(vm_value_t* argv, size_t argc, void* vm);
vm_value_t vm_array_init(vm_value_t* argv, size_t argc, void* vm);
vm_value_t vm_array_get(vm_value_t* argv, size_t argc, void* vm);
vm_value_t vm_array_length(vm_value_t* argv, size_t argc, void* vm);
vm_value_t vm_array_set(vm_value_t* argv, size_t argc, void* vm);
vm_value_t vm_array_push(vm_value_t* argv, size_t argc, void* vm);

