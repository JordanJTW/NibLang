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

Map* init_map(size_t bucket_count) {
  Map* map = (Map*)calloc(1, sizeof(Map) + sizeof(MapNode*) * bucket_count);
  if (map == NULL)
    return NULL;
  map->bucket_count = bucket_count;
  return map;
}

void free_map(Map* map) {
  for (size_t i = 0; i < map->bucket_count; ++i) {
    MapNode* node = map->buckets[i];
    while (node) {
      MapNode* next = node->next;
      free(node);
      node = next;
    }
  }
  free(map);
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
  // by type provides some consistency.
  if (v1.type != v2.type)
    return v1.type - v2.type;

  switch (v1.type) {
    case VALUE_TYPE_NULL:
    case VALUE_TYPE_BOOL:
    case VALUE_TYPE_FLOAT:
    case VALUE_TYPE_FUNCTION:
    case VALUE_TYPE_PROMISE:
      return 0;
    case VALUE_TYPE_MAP:
      return v1.as.map->id - v2.as.map->id;
    case VALUE_TYPE_INT:
      return v1.as.i32 - v2.as.i32;
    case VALUE_TYPE_STR:
      return strcmp(v1.as.str->c_str, v2.as.str->c_str);
  }
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
    *found_node = value;
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

void delete_map(void* self) {
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
  free(map);
}

vm_value_t allocate_map() {
  Map* map = init_map(/*bucket_count=*/13);
  if (map == NULL) {
    return (vm_value_t){.type = VALUE_TYPE_NULL};
  }
  map->ref_count.deleter = &delete_map;
  static uint32_t next_id = 0;
  map->id = ++next_id;
  return (vm_value_t){.type = VALUE_TYPE_MAP, .as.map = map};
}

vm_value_t vm_map_set(vm_value_t* argv, size_t argc, void* vm) {
  assert(argc == 3 && is_map(argv[0]) &&
         "incorrect number of args or arg types");

  RC_AUTOFREE vm_value_t this = argv[0];
  map_insert(this.as.map, argv[1], argv[2]);
  return (vm_value_t){.type = VALUE_TYPE_NULL};
}

static void DumpValue(vm_value_t value) {
  switch (value.type) {
    case VALUE_TYPE_BOOL:
      printf(value.as.boolean ? "true" : "false");
      break;
    case VALUE_TYPE_FLOAT:
      printf("%f", value.as.f32);
      break;
    case VALUE_TYPE_FUNCTION:
      printf("Function [%s]", value.as.fn->fn->name);
      break;
    case VALUE_TYPE_INT:
      printf("%d", value.as.i32);
      break;
    case VALUE_TYPE_MAP:
      DumpMap(value.as.map);
      break;
    case VALUE_TYPE_NULL:
      printf("(null)");
      break;
    case VALUE_TYPE_PROMISE:
      printf("Promise <");
      DumpValue(value.as.promise->value);
      printf(">");
      break;
    case VALUE_TYPE_STR:
      printf("\"%.*s\"", (int)value.as.str->len, value.as.str->c_str);
      break;
  }
}

void DumpMap(Map* map) {
  printf("Map {\n");
  for (size_t i = 0; i < map->bucket_count; ++i) {
    for (MapNode* node = map->buckets[i]; node; node = node->next) {
      printf("key: ");
      DumpValue(node->key);
      printf(" value: ");
      DumpValue(node->value);
      printf(",\n");
    }
  }
  printf("}\n");
}