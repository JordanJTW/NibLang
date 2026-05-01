
// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum : uint8_t {
  // Variables
  OP_PUSH_CONST_REF,
  OP_PUSH_I32,
  OP_PUSH_F32,
  OP_PUSH_TRUE,
  OP_PUSH_FALSE,
  OP_PUSH_NULL,
  OP_PUSH_LOCAL,
  OP_STORE_LOCAL,
  OP_STACK_DUP,
  OP_STACK_DEL,
  // Functions
  OP_CALL,
  OP_DYNAMIC_CALL,
  OP_RETURN,
  OP_BIND,
  // Operators
  OP_ADD,
  OP_SUB,
  OP_MUL,
  OP_DIV,
  OP_AND,
  OP_OR,
  OP_NOT,
  OP_INC,
  OP_CONCAT,
  // Comparisons
  OP_LESS_THAN,
  OP_LESS_OR_EQ,
  OP_EQUAL,
  OP_GREAT_OR_EQ,
  OP_GREATER_THAN,
  OP_IS,
  // Control Flow
  OP_JUMP_IF_TRUE,
  OP_JUMP_IF_FALSE,
  OP_JUMP,
  // Exception
  OP_TRY_PUSH,
  OP_TRY_POP,
  OP_THROW,

  OP_DEBUG,
} op_t;

typedef struct vm_t vm_t;
typedef struct vm_job_queue_t vm_job_queue_t;
typedef struct MapNode MapNode;
typedef struct vm_promise_t Promise;
typedef struct vm_array_t Array;
typedef struct vm_closure_t Closure;
typedef struct vm_allocation_t vm_allocation_t;

typedef struct ref_count_t {
  uint32_t count;
  void (*deleter)(void* self, bool free);
  vm_allocation_t* allocation;
} ref_count_t;

struct vm_allocation_t {
  struct vm_allocation_t* next;
  ref_count_t* object;
  size_t allocation_size;
  char* from_name;
  size_t from_line;
};

typedef struct {
  vm_allocation_t* allocation_head;
} vm_gc_t;

typedef struct vm_string_t {
  ref_count_t rc;
  size_t len;
  char c_str[];
} String;

typedef struct vm_map_t {
  ref_count_t rc;
  uint32_t id;
  size_t element_count;
  size_t bucket_count;
  MapNode* buckets[];
} Map;

typedef struct vm_value {
  enum type_t {
    VALUE_TYPE_VOID,  // Special return value for functions
    VALUE_TYPE_NULL,
    VALUE_TYPE_BOOL,
    VALUE_TYPE_INT,
    VALUE_TYPE_FLOAT,
    VALUE_TYPE_STR,
    VALUE_TYPE_MAP,
    VALUE_TYPE_FUNCTION,
    VALUE_TYPE_PROMISE,
    VALUE_TYPE_ARRAY,
    VALUE_TYPE_OPAQUE,
  } type;
  union {
    bool boolean;
    int32_t i32;
    float f32;
    String* str;
    Map* map;
    Closure* fn;
    Promise* promise;
    Array* array;
    // All reference (heap-allocated) values start with a `ref_count_t`
    ref_count_t* ref;
  } as;
} vm_value_t;

typedef struct vm_promise_then_t vm_promise_then_t;

typedef struct vm_promise_t {
  ref_count_t rc;
  enum state_t {
    PROMISE_STATE_PENDING,
    PROMISE_STATE_FULFILLED,
    PROMISE_STATE_REJECTED,
  } state;
  vm_value_t value;
  vm_promise_then_t* then_list;
} Promise;

typedef struct vm_array_t {
  ref_count_t rc;
  size_t len;
  size_t capacity;
  vm_value_t* data;
} Array;

#ifdef __cplusplus
}  // extern "C"
#endif