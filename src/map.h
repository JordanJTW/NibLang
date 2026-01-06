#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "src/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MapNode {
  vm_value_t key;
  vm_value_t value;
  struct MapNode* next;
} MapNode;

Map* init_map(size_t bucket_count);
void free_map(Map* map);

vm_value_t* map_get(Map* map, vm_value_t key);
bool map_insert(Map* map, vm_value_t key, vm_value_t value);
bool map_has_key(Map* map, vm_value_t key);
bool map_remove(Map* map, vm_value_t key);

vm_value_t allocate_map(uint32_t bucket_count);

#ifdef __cplusplus
}  // extern "C"
#endif