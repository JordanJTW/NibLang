#pragma once

#include <stddef.h>

#include "src/types.h"

vm_value_t vm_strings_substring(vm_value_t* argv, size_t argc, void*);
vm_value_t vm_strings_get(vm_value_t* argv, size_t argc, void*);