// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <stddef.h>

#include "src/types.h"

vm_value_t vm_strings_substring(vm_value_t* argv, size_t argc, void*);
vm_value_t vm_strings_get(vm_value_t* argv, size_t argc, void*);
vm_value_t vm_strings_starts_with(vm_value_t* argv, size_t argc, void*);
vm_value_t vm_string_length(vm_value_t* argv, size_t argc, void*);