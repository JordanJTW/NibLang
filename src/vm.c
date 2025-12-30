#include "vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/op.h"

#ifdef NDEBUG
#define DEBUG_LOG($fmt, ...)
#else
#define DEBUG_LOG($fmt, ...) \
  fprintf(stderr, "[%s:%d] " $fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)
#endif  // NDEBUG

typedef struct vm_frame vm_frame_t;

static void increment_refcount(vm_value_t* value);

typedef struct vm_string_t {
  ref_count_t ref_count;
  size_t len;
  char c_str[];
} String;

typedef struct {
  vm_value_t* contants;
  size_t contants_count;
  vm_func_t* functions;
  size_t function_count;
  vm_native_func_t* native_functions;
  size_t native_function_count;
  vm_frame_t* current_frame;
  vm_stack_t stack;
} __vm_t;

typedef struct vm_frame {
  vm_func_t* func;
  size_t pc;
  vm_frame_t* next;
  vm_value_t locals[];
} vm_frame_t;

vm_t new_vm(vm_value_t* constants,
            size_t constants_count,
            vm_func_t* functions,
            size_t functions_count,
            vm_native_func_t* native_functions,
            size_t native_functions_count) {
  __vm_t* vm = calloc(1, sizeof(__vm_t));
  vm->contants = constants;
  vm->contants_count = constants_count;
  vm->functions = functions;
  vm->function_count = functions_count;
  vm->native_functions = native_functions;
  vm->native_function_count = native_functions_count;
  vm->stack.capacity = 64;
  vm->stack.values = calloc(vm->stack.capacity, sizeof(vm_value_t));
  return vm;
}

void free_vm(vm_t vm) {
  __vm_t* _vm = (__vm_t*)vm;
  for (size_t i = 0; i < _vm->contants_count; ++i)
    if (_vm->contants[i].type == VALUE_TYPE_STR)
      free(_vm->contants[i].as.str);

  free(_vm->stack.values);
  free(_vm);
}

static void increment_refcount(vm_value_t* value) {
  switch (value->type) {
    case VALUE_TYPE_NULL:
    case VALUE_TYPE_INT:
      return;

    case VALUE_TYPE_STR: {
      ++(*value->as.ref);
      break;
    }
  }
}

static void decrement_refcount(vm_value_t* value) {
  switch (value->type) {
    case VALUE_TYPE_NULL:
    case VALUE_TYPE_INT:
      return;

    case VALUE_TYPE_STR: {
      --(*value->as.ref);
      if (*value->as.ref == 0)
        free(value->as.ref);
      break;
    }
  }
}

// Pushes `value` on to the stack. The stack takes ownership of `value`.
static void push_stack(vm_stack_t* stack, vm_value_t value) {
  assert(stack->sp >= stack->capacity || "stack overflow");
  increment_refcount(&value);
  stack->values[stack->sp++] = value;
}

// Pops the top of the stack.
// NOTE: Transfers the stack's ownership of the value to the caller!
static vm_value_t pop_stack(vm_stack_t* stack) {
  assert(stack->sp >= 1 || "stack underflow");
  return stack->values[--stack->sp];
}

static void run_frame(__vm_t* vm) {
#define CHECK_BOUNDS($pc)                                          \
  if ($pc >= frame->func->data_len) {                              \
    DEBUG_LOG("data overflow in func: '%s'\n", frame->func->name); \
    return;                                                        \
  }

  while (1) {
    vm_frame_t* frame = vm->current_frame;

    CHECK_BOUNDS(frame->pc);

    switch (frame->func->data[frame->pc]) {
      case OP_PUSH_CONST_REF: {
        CHECK_BOUNDS(frame->pc + 1);
        uint8_t const_idx = frame->func->data[frame->pc + 1];
        DEBUG_LOG("OP_PUSH_CONST_REF idx: %d", const_idx);
        assert(vm->contants_count > const_idx || "invalid const");
        push_stack(&vm->stack, vm->contants[const_idx]);
        frame->pc += 2;
        break;
      }
      case OP_PUSH_CONST: {
        CHECK_BOUNDS(frame->pc + 1);
        uint8_t value = frame->func->data[frame->pc + 1];
        DEBUG_LOG("OP_PUSH_CONST value: %d", value);
        push_stack(&vm->stack,
                   (vm_value_t){.type = VALUE_TYPE_INT, .as.i32 = value});
        frame->pc += 2;
        break;
      }
      case OP_CALL: {
        CHECK_BOUNDS(frame->pc + 1);
        uint8_t func_idx = frame->func->data[frame->pc + 1];
        assert(vm->function_count > func_idx || "invalid func");
        vm_func_t* func = &vm->functions[func_idx];
        DEBUG_LOG("OP_CALL idx: %d (%s)", func_idx, func->name);
        vm_frame_t* new_frame = calloc(
            1, sizeof(vm_frame_t) + sizeof(vm_value_t) * func->local_count);

        new_frame->func = func;
        for (size_t arg_idx = 0; arg_idx < func->arg_count; ++arg_idx) {
          new_frame->locals[arg_idx] = pop_stack(&vm->stack);
        }
        new_frame->next = vm->current_frame;
        vm->current_frame = new_frame;
        frame->pc += 2;
        break;
      }
      case OP_PUSH_LOCAL: {
        CHECK_BOUNDS(frame->pc + 1);
        uint8_t local_idx = frame->func->data[frame->pc + 1];
        DEBUG_LOG("OP_PUSH_LOCAL idx: %d", local_idx);
        assert(frame->func->local_count > local_idx || "invalid local");
        vm_value_t value = vm->current_frame->locals[local_idx];
        push_stack(&vm->stack, value);
        frame->pc += 2;
        break;
      }
      case OP_ADD: {
        vm_value_t arg1 = pop_stack(&vm->stack);
        vm_value_t arg2 = pop_stack(&vm->stack);
        DEBUG_LOG("OP_ADD %d + %d", arg1.as.i32, arg2.as.i32);

        assert(arg1.type == VALUE_TYPE_INT && arg2.type == VALUE_TYPE_INT ||
               "only ints supported for add");

        push_stack(&vm->stack,
                   (vm_value_t){.type = VALUE_TYPE_INT,
                                .as.i32 = (arg1.as.i32 + arg2.as.i32)});
        ++frame->pc;
        break;
      }
      case OP_STORE_LOCAL: {
        CHECK_BOUNDS(frame->pc + 1);
        uint8_t local_idx = frame->func->data[frame->pc + 1];
        DEBUG_LOG("OP_STORE_LOCAL idx: %d", local_idx);
        assert(frame->func->local_count > local_idx || "invalid local");
        decrement_refcount(&frame->locals[local_idx]);
        frame->locals[local_idx] = pop_stack(&vm->stack);
        frame->pc += 2;
        break;
      }
      case OP_CALL_NATIVE: {
        CHECK_BOUNDS(frame->pc + 1);
        uint8_t native_idx = frame->func->data[frame->pc + 1];
        assert(vm->native_function_count > native_idx || "invalid func");
        vm_native_func_t native_func = vm->native_functions[native_idx];
        DEBUG_LOG("OP_CALL_NATIVE idx: %d (%s)", native_idx, native_func.name);

        assert(vm->stack.sp > native_func.arg_count || "stack overflow");

        vm_value_t result = native_func.func(
            vm->stack.values + vm->stack.sp - native_func.arg_count,
            native_func.arg_count, native_func.userdata);

        vm->stack.sp -= native_func.arg_count;
        if (result.type != VALUE_TYPE_NULL)
          push_stack(&vm->stack, result);
        frame->pc += 2;
        break;
      }
      case OP_RETURN: {
        DEBUG_LOG("OP_RETURN");
        vm_frame_t* current_frame = vm->current_frame;
        vm->current_frame = current_frame->next;

        for (size_t i = 0; i < current_frame->func->local_count; ++i)
          decrement_refcount(&current_frame->locals[i]);

        free(current_frame);
        if (vm->current_frame == NULL)  // Nothing to return to... exit.
          return;
        break;
      }
#define EQUALITY_CASE($op, $condition)                                    \
  case $op: {                                                             \
    vm_value_t v2 = pop_stack(&vm->stack);                                \
    vm_value_t v1 = pop_stack(&vm->stack);                                \
    assert(v1.type == VALUE_TYPE_INT && v2.type == VALUE_TYPE_INT ||      \
           "only i32 are comparable");                                    \
    DEBUG_LOG(#$op " %d" #$condition "%d", v1.as.i32, v2.as.i32);         \
    push_stack(&vm->stack,                                                \
               (vm_value_t){.type = VALUE_TYPE_INT,                       \
                            .as.i32 = (v1.as.i32 $condition v2.as.i32)}); \
    ++frame->pc;                                                          \
    break;                                                                \
  }

        EQUALITY_CASE(OP_LESS_THAN, <)
        EQUALITY_CASE(OP_LESS_OR_EQ, <=)
        EQUALITY_CASE(OP_EQUAL, ==)
        EQUALITY_CASE(OP_GREAT_OR_EQ, >=)
        EQUALITY_CASE(OP_GREATER_THAN, >)

#undef EQUALITY_CASE

      case OP_JUMP_IF_FALSE: {
        CHECK_BOUNDS(frame->pc + 1);
        uint8_t address = frame->func->data[frame->pc + 1];
        DEBUG_LOG("OP_JUMP_IF_FALSE to 0x%02x", address);
        vm_value_t condition = pop_stack(&vm->stack);
        assert(condition.type == VALUE_TYPE_INT || "condition must be i32");
        if (condition.as.i32 == 0) {
          frame->pc = address;
        } else {
          frame->pc += 2;
        }
        break;
      }

      case OP_JUMP: {
        CHECK_BOUNDS(frame->pc + 1);
        uint8_t address = frame->func->data[frame->pc + 1];
        DEBUG_LOG("OP_JUMP to 0x%02x", address);
        frame->pc = address;
        break;
      }

      default: {
        DEBUG_LOG("unknown op-code: 0x%02x", frame->func->data[frame->pc]);
        return;
      }
    }
  }
}

void vm_run(vm_t vm) {
  __vm_t* __vm = (__vm_t*)vm;

  assert(__vm->function_count >= 1 || "must have main() function");

  vm_func_t* func = &__vm->functions[0];
  vm_frame_t* frame =
      calloc(1, sizeof(vm_frame_t) + sizeof(vm_value_t) * func->local_count);

  frame->func = func;
  __vm->current_frame = frame;
  run_frame(__vm);
}

bool vm_as_int32(vm_value_t* value, int32_t* out) {
  if (value == NULL || value->type != VALUE_TYPE_INT)
    return false;
  *out = value->as.i32;
  return true;
}

typedef uint32_t ref_count_t;

vm_value_t allocate_str_from_c(const char* str) {
  return allocate_str_from_c_with_length(str, strlen(str));
}

vm_value_t allocate_str_from_c_with_length(const char* str, size_t length) {
  String* string = calloc(1, sizeof(String) + length + 1);
  string->len = length;
  memcpy(string->c_str, str, length + 1);  // Add in the \0 terminator
  return (vm_value_t){.type = VALUE_TYPE_STR, .as.str = string};
}

size_t vm_as_str(vm_value_t* value, char** out) {
  if (value == NULL || value->type != VALUE_TYPE_STR)
    return 0;
  *out = value->as.str->c_str;
  return value->as.str->len;
}

void vm_free_ref(vm_value_t value) {
  decrement_refcount(&value);
}