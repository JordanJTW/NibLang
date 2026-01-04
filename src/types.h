#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum : uint8_t {
  // Variables
  OP_PUSH_CONST_REF,
  OP_PUSH_CONST,
  OP_PUSH_LOCAL,
  OP_STORE_LOCAL,
  // Functions
  OP_CALL,
  OP_RETURN,
  OP_CALL_NATIVE,
  // Operators
  OP_ADD,
  OP_SUB,
  OP_MUL,
  OP_DIV,
  OP_AND,
  OP_OR,
  OP_NOT,
  // Comparisons
  OP_LESS_THAN,
  OP_LESS_OR_EQ,
  OP_EQUAL,
  OP_GREAT_OR_EQ,
  OP_GREATER_THAN,
  // Control Flow
  OP_JUMP_IF_FALSE,
  OP_JUMP,
} op_t;

typedef uint32_t ref_count_t;
typedef struct MapNode MapNode;

typedef struct vm_string_t {
  ref_count_t ref_count;
  size_t len;
  char c_str[];
} String;

typedef struct vm_map_t {
  ref_count_t ref_count;
  uint32_t id;
  size_t element_count;
  size_t bucket_count;
  MapNode* buckets[];
} Map;

typedef struct vm_value {
  enum {
    VALUE_TYPE_NULL,
    VALUE_TYPE_BOOL,
    VALUE_TYPE_INT,
    VALUE_TYPE_FLOAT,
    VALUE_TYPE_STR,
    VALUE_TYPE_MAP,
  } type;
  union {
    bool boolean;
    int32_t i32;
    float f32;
    String* str;
    Map* map;
    // All reference (heap-allocated) values start with a `ref_count_t`
    ref_count_t* ref;
  } as;
} vm_value_t;