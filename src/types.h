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
  OP_PUSH_LOCAL,
  OP_STORE_LOCAL,
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
  // Comparisons
  OP_LESS_THAN,
  OP_LESS_OR_EQ,
  OP_EQUAL,
  OP_GREAT_OR_EQ,
  OP_GREATER_THAN,
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

typedef struct ref_count_t {
  uint32_t count;
  void (*deleter)(void* self);
} ref_count_t;

typedef struct vm_string_t {
  ref_count_t rc;
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
    VALUE_TYPE_FUNCTION,
    VALUE_TYPE_PROMISE,
    VALUE_TYPE_ARRAY,
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
  ref_count_t ref_count;
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