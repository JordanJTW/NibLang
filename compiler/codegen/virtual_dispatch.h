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

#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <vector>

struct FunctionSymbol;

struct VirtualDispatchResult {
  std::vector<uint32_t> row_displacements;
  std::vector<uint32_t> dispatch_table;
  std::vector<uint32_t> slot_owners;

  std::unordered_map<uint32_t, uint32_t> symbol_to_object_id;
  std::unordered_map<uint32_t, uint32_t> symbol_to_interface_id;
};

// Builds a compressed dispatch table from `interface_functions`.
// `symbol_to_func_idx` provides the mappings from FunctionSymbols to
// their index in the function table.
VirtualDispatchResult BuildDispatchTable(
    const std::vector<const FunctionSymbol*>& interface_functions,
    const std::unordered_map<uint32_t, uint32_t>& symbol_to_func_idx);

void PrintDispatchTable(const std::vector<uint32_t>& row_displacements,
                        const std::vector<uint32_t>& dispatch_table,
                        std::ostream& os = std::cout);