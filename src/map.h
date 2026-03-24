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

vm_value_t allocate_map(vm_gc_t* gc, size_t bucket_count);

vm_value_t* map_get(Map* map, vm_value_t key);
bool map_insert(Map* map, vm_value_t key, vm_value_t value);
bool map_has_key(Map* map, vm_value_t key);
bool map_remove(Map* map, vm_value_t key);

vm_value_t vm_map_alloc(vm_value_t* argv, size_t argc, void*);
vm_value_t vm_map_get(vm_value_t* argv, size_t argc, void*);
vm_value_t vm_map_set(vm_value_t* argv, size_t argc, void*);

#ifdef __cplusplus
}  // extern "C"
#endif