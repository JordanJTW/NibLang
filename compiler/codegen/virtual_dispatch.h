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