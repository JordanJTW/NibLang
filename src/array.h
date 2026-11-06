/*
 * Copyright (C) 2026, Jordan Werthman <jordanwerthman@gmail.com>.
 *
 * This software is dual-licensed. You may use this file under the terms of
 * the GNU General Public License v3.0 (GPL-3.0) OR under a commercial
 * license obtained directly from the copyright holder.
 *
 * See the LICENSE.md file in the project root for full licensing details.
 *
 * SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-Commercial
 */

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
