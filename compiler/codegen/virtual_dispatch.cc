#include "compiler/codegen/virtual_dispatch.h"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <ranges>

#include "compiler/logging.h"
#include "compiler/symbol_types.h"

constexpr uint32_t EMPTY_SLOT = std::numeric_limits<uint32_t>::max();

VirtualDispatchResult BuildDispatchTable(
    const std::vector<const FunctionSymbol*>& interface_functions,
    const std::unordered_map<uint32_t, uint32_t>& symbol_to_func_idx) {
  VirtualDispatchResult result;

  std::vector<const FunctionSymbol*> sorted_methods;
  sorted_methods.reserve(interface_functions.size());
  for (const auto* fn : interface_functions) {
    CHECK(fn);
    sorted_methods.push_back(fn);
  }

  std::ranges::sort(
      sorted_methods, [](const FunctionSymbol* a, const FunctionSymbol* b) {
        if (a->implementations.size() != b->implementations.size())
          return a->implementations.size() > b->implementations.size();

        return a->symbol_id < b->symbol_id;
      });

  for (uint32_t col_id = 0; col_id < sorted_methods.size(); ++col_id) {
    result.symbol_to_interface_id[sorted_methods[col_id]->symbol_id] = col_id;
  }

  struct StructRow {
    SymbolId struct_symbol_id;
    std::vector<std::pair<uint32_t, uint32_t>> entries;
  };

  std::unordered_map<SymbolId, StructRow> struct_rows_map;
  for (const auto* fn : sorted_methods) {
    uint32_t col_id = result.symbol_to_interface_id[fn->symbol_id];

    for (const auto& [struct_symbol_id, impl_symbol_id] : fn->implementations) {
      auto it = symbol_to_func_idx.find(impl_symbol_id);
      if (it == symbol_to_func_idx.end())
        continue;

      uint32_t func_idx = it->second;
      auto& row = struct_rows_map[struct_symbol_id];
      row.struct_symbol_id = struct_symbol_id;
      row.entries.emplace_back(col_id, func_idx);
    }
  }

  std::vector<StructRow> sorted_rows;
  sorted_rows.reserve(struct_rows_map.size());
  for (auto& row : struct_rows_map | std::views::values) {
    if (!row.entries.empty())
      sorted_rows.push_back(std::move(row));
  }

  std::ranges::sort(sorted_rows, [](const StructRow& a, const StructRow& b) {
    if (a.entries.size() != b.entries.size())
      return a.entries.size() > b.entries.size();

    return a.struct_symbol_id < b.struct_symbol_id;
  });

  result.row_displacements.resize(sorted_rows.size(), 0);
  for (uint32_t dense_id = 0; dense_id < sorted_rows.size(); ++dense_id) {
    result.symbol_to_object_id[sorted_rows[dense_id].struct_symbol_id] =
        dense_id;
  }

  for (uint32_t object_id = 0; object_id < sorted_rows.size(); ++object_id) {
    const auto& [struct_symbol_id, entries] = sorted_rows[object_id];

    uint32_t shift = 0;
    bool fits = false;

    while (!fits) {
      fits = true;
      for (const auto& col_id : entries | std::views::keys) {
        uint32_t target_slot = shift + col_id;

        if (target_slot < result.dispatch_table.size() &&
            result.dispatch_table[target_slot] != EMPTY_SLOT) {
          fits = false;
          break;
        }
      }

      if (!fits)
        shift++;
    }

    result.row_displacements[object_id] = shift;

    uint32_t max_slot = 0;
    for (const auto& col_id : entries | std::views::keys) {
      max_slot = std::max(max_slot, shift + col_id);
    }

    if (max_slot >= result.dispatch_table.size()) {
      result.dispatch_table.resize(max_slot + 1, EMPTY_SLOT);
      result.slot_owners.resize(max_slot + 1, EMPTY_SLOT);
    }

    for (const auto& [col_id, func_idx] : entries) {
      uint32_t slot = shift + col_id;
      result.dispatch_table[slot] = func_idx;
      result.slot_owners[slot] = object_id;
    }
  }

  return result;
}

void PrintDispatchTable(const std::vector<uint32_t>& row_displacements,
                        const std::vector<uint32_t>& dispatch_table,
                        std::ostream& os) {
  const size_t num_slots = dispatch_table.size() / 2;

  size_t non_empty = 0;
  for (size_t i = 0; i < num_slots; ++i) {
    if (dispatch_table[i * 2] != EMPTY_SLOT)
      non_empty++;
  }

  const double density =
      (num_slots == 0) ? 0.0
                       : (static_cast<double>(non_empty) / num_slots) * 100.0;

  os << "=== VIRTUAL DISPATCH TABLE ===\n";
  os << "Structs: " << row_displacements.size() << " | Slots: " << num_slots
     << " | Density: " << std::fixed << std::setprecision(1) << density << "% ("
     << non_empty << "/" << num_slots << ")\n";

  // Displacements inline
  os << "Displacements: ";
  for (size_t i = 0; i < row_displacements.size(); ++i) {
    os << "s#" << i << ":+" << row_displacements[i]
       << (i + 1 < row_displacements.size() ? ", " : "\n");
  }

  // Flat Table (All on one line)
  os << "Flat Table: ";
  for (size_t i = 0; i < num_slots; ++i) {
    uint32_t fn_idx = dispatch_table[i * 2];
    uint32_t owner_id = dispatch_table[i * 2 + 1];

    os << "[" << i << "]:";
    if (fn_idx == EMPTY_SLOT) {
      os << "_";
    } else {
      os << "#" << owner_id << ":@" << fn_idx;
    }
    if (i + 1 < num_slots)
      os << " ";
  }
  os << "\n";

  // 2D Matrix (shows only @id)
  if (!row_displacements.empty() && num_slots > 0) {
    uint32_t max_col = 0;
    for (size_t slot = 0; slot < num_slots; ++slot) {
      uint32_t fn_idx = dispatch_table[slot * 2];
      uint32_t owner_id = dispatch_table[slot * 2 + 1];
      if (fn_idx != EMPTY_SLOT && owner_id < row_displacements.size()) {
        uint32_t disp = row_displacements[owner_id];
        if (slot >= disp) {
          max_col = std::max(max_col, static_cast<uint32_t>(slot - disp));
        }
      }
    }

    constexpr int kColWidth = 6;
    constexpr int kLabelWidth = 6;

    os << "Matrix:\n";
    os << std::left << std::setw(kLabelWidth) << " ";
    for (uint32_t col = 0; col <= max_col; ++col) {
      os << std::left << std::setw(kColWidth) << ("I" + std::to_string(col));
    }
    os << "\n";

    for (size_t dense_id = 0; dense_id < row_displacements.size(); ++dense_id) {
      uint32_t disp = row_displacements[dense_id];
      os << std::left << std::setw(kLabelWidth)
         << ("s#" + std::to_string(dense_id) + ":");

      for (uint32_t col = 0; col <= max_col; ++col) {
        size_t slot = disp + col;
        if (slot < num_slots && dispatch_table[slot * 2] != EMPTY_SLOT &&
            dispatch_table[slot * 2 + 1] == dense_id) {
          std::string cell = "@" + std::to_string(dispatch_table[slot * 2]);
          os << std::left << std::setw(kColWidth) << cell;
        } else {
          os << std::left << std::setw(kColWidth) << "_";
        }
      }
      os << "\n";
    }
  }
}