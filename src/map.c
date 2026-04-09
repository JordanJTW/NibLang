#include "src/map.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/types.h"
#include "src/vm.h"

static inline bool is_map(vm_value_t value) {
  return value.type == VALUE_TYPE_MAP;
}

void free_map(void* self, bool should_free) {
  Map* map = self;
  for (size_t i = 0; i < map->bucket_count; ++i) {
    MapNode* node = map->buckets[i];
    while (node) {
      MapNode* next = node->next;
      vm_free_ref(&node->key);
      vm_free_ref(&node->value);
      free(node);
      node = next;
    }
  }
  if (should_free)
    free(self);
}

vm_value_t allocate_map(vm_gc_t* gc, size_t bucket_count) {
  Map* map = vm_gc_allocate(gc, sizeof(Map) + sizeof(MapNode*) * bucket_count);
  if (map == NULL)
    return (vm_value_t){.type = VALUE_TYPE_NULL};

  map->bucket_count = bucket_count;
  map->rc.deleter = &free_map;
  static uint32_t next_id = 0;
  map->id = ++next_id;

  vm_value_t value = (vm_value_t){.type = VALUE_TYPE_MAP, .as.map = map};
  vm_adopt_ref(value);
  return value;
}

static uint32_t hash(vm_value_t value) {
  switch (value.type) {
    case VALUE_TYPE_FLOAT:
      return (uint32_t)value.as.f32;
    case VALUE_TYPE_INT:
      return (uint32_t)value.as.i32;
    case VALUE_TYPE_MAP:
      return (uint32_t)value.as.map->id;
    case VALUE_TYPE_STR: {
      uint32_t hash = 7;
      for (size_t i = 0; i < value.as.str->len; i++) {
        hash = hash * 31 + value.as.str->c_str[i];
      }
      return hash;
    }
    default:
      fprintf(stderr, "non-hashable value type: %d\n", value.type);
      abort();
      return 0;
  }
}

static int compare(vm_value_t v1, vm_value_t v2) {
  // There is no type coercion so unlike types will never be equal BUT ordering
  // by type provides consistency.
  if (v1.type != v2.type)
    return v1.type - v2.type;

  switch (v1.type) {
    case VALUE_TYPE_VOID:
      assert(false && "VALUE_TYPE_VOID should never appear on the stack");
    case VALUE_TYPE_NULL:
    case VALUE_TYPE_BOOL:
    case VALUE_TYPE_FLOAT:
    case VALUE_TYPE_FUNCTION:
    case VALUE_TYPE_PROMISE:
    case VALUE_TYPE_ARRAY:
    case VALUE_TYPE_OPAQUE:
      break;
    case VALUE_TYPE_MAP:
      return v1.as.map->id - v2.as.map->id;
    case VALUE_TYPE_INT:
      return v1.as.i32 - v2.as.i32;
    case VALUE_TYPE_STR:
      return strcmp(v1.as.str->c_str, v2.as.str->c_str);
  }
  return 0;
}

vm_value_t* map_get(Map* map, vm_value_t key) {
  uint32_t bucket_idx = hash(key) % map->bucket_count;
  for (MapNode* node = map->buckets[bucket_idx]; node; node = node->next) {
    if (compare(key, node->key) == 0)
      return &node->value;
  }
  return NULL;
}

bool map_insert(Map* map, vm_value_t key, vm_value_t value) {
  vm_value_t* found_node = map_get(map, key);
  if (found_node != NULL) {
    vm_free_ref(&key);  // Already have `key` owned by map!

    vm_value_t existing_value = *found_node;
    *found_node = value;
    vm_free_ref(&existing_value);
    return false;
  }

  uint32_t bucket_idx = hash(key) % map->bucket_count;

  MapNode* node = malloc(sizeof(MapNode));
  node->key = key;
  node->value = value;
  node->next = map->buckets[bucket_idx];
  map->buckets[bucket_idx] = node;
  map->element_count++;
  return true;
}

bool map_has_key(Map* map, vm_value_t key) {
  return map_get(map, key) != NULL;
}

bool map_remove(Map* map, vm_value_t key) {
  uint32_t bucket_idx = hash(key) % map->bucket_count;
  MapNode** prev = &map->buckets[bucket_idx];
  for (MapNode* node = map->buckets[bucket_idx]; node; node = node->next) {
    if (compare(key, node->key) == 0) {
      *prev = node->next;
      free(node);
      map->element_count--;
      return true;
    }
    prev = &node->next;
  }
  return false;
}

vm_value_t vm_map_alloc(vm_value_t* argv, size_t argc, void* vm) {
  return allocate_map(vm_get_gc(vm), 13);
}

vm_value_t vm_map_get(vm_value_t* argv, size_t argc, void* vm) {
  assert(argc == 2 && is_map(argv[0]) &&
         "incorrect number of args or arg types");

  RC_AUTOFREE vm_value_t this = argv[0];
  RC_AUTOFREE vm_value_t key = argv[1];

  vm_value_t* value = map_get(this.as.map, key);
  if (value == NULL)
    return (vm_value_t){.type = VALUE_TYPE_NULL};

  vm_adopt_ref(*value);
  return *value;
}

vm_value_t vm_map_set(vm_value_t* argv, size_t argc, void* vm) {
  assert(argc == 3 && is_map(argv[0]) &&
         "incorrect number of args or arg types");

  RC_AUTOFREE vm_value_t this = argv[0];

  map_insert(this.as.map, argv[1], argv[2]);
  return (vm_value_t){.type = VALUE_TYPE_VOID};
}
