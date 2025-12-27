#include "vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct vm_frame vm_frame_t;

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

typedef struct vm_value {
  enum {
    VALUE_TYPE_INT,
    VALUE_TYPE_REF,
  } type;
  union {
    int32_t i32;
    void* ref;
  } as;
} value_t;

typedef struct vm_frame {
  vm_func_t* func;
  size_t pc;
  vm_frame_t* next;
  vm_value_t locals[];
} vm_frame_t;

typedef enum {
  PUSH_CONST = 0x01,
  CALL = 0x02,
  PUSH_LOCAL = 0x03,
  ADD = 0x04,
  LOAD_LOCAL = 0x07,
  CALL_NATIVE = 0x06,
  RETURN = 0x05,
} op_t;

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
  free((__vm_t*)vm);
}

static void push_stack(vm_stack_t* stack, value_t value) {
  assert(stack->sp >= stack->capacity || "stack overflow");
  stack->values[stack->sp++] = value;
}

static vm_value_t pop_stack(vm_stack_t* stack) {
  assert(stack->sp >= 1 || "stack underflow");
  return stack->values[--stack->sp];
}

static void run_frame(__vm_t* vm) {
#define CHECK_BOUNDS($pc)                                                \
  if ($pc >= frame->func->data_len) {                                    \
    fprintf(stderr, "data overflow in func: '%s'\n", frame->func->name); \
    return;                                                              \
  }

  while (1) {
    vm_frame_t* frame = vm->current_frame;

    CHECK_BOUNDS(frame->pc);

    switch (frame->func->data[frame->pc]) {
      case PUSH_CONST: {
        CHECK_BOUNDS(frame->pc + 1);
        uint8_t value = frame->func->data[frame->pc + 1];
        fprintf(stderr, "PUSH_CONST value: %d\n", value);
        push_stack(&vm->stack,
                   (value_t){.type = VALUE_TYPE_INT, .as.i32 = value});
        frame->pc += 2;
        break;
      }
      case CALL: {
        CHECK_BOUNDS(frame->pc + 1);
        uint8_t func_idx = frame->func->data[frame->pc + 1];
        assert(vm->function_count > func_idx || "invalid func");
        vm_func_t* func = &vm->functions[func_idx];
        fprintf(stderr, "CALL idx: %d (%s)\n", func_idx, func->name);
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
      case PUSH_LOCAL: {
        CHECK_BOUNDS(frame->pc + 1);
        uint8_t local_idx = frame->func->data[frame->pc + 1];
        fprintf(stderr, "PUSH_LOCAL idx: %d\n", local_idx);
        assert(frame->func->local_count > local_idx || "invalid local");
        vm_value_t value = vm->current_frame->locals[local_idx];
        push_stack(&vm->stack, value);
        frame->pc += 2;
        break;
      }
      case ADD: {
        value_t arg1 = pop_stack(&vm->stack);
        value_t arg2 = pop_stack(&vm->stack);
        fprintf(stderr, "ADD %d + %d\n", arg1.as.i32, arg2.as.i32);

        assert(arg1.type == VALUE_TYPE_INT && arg2.type == VALUE_TYPE_INT ||
               "only ints supported for add");

        push_stack(&vm->stack,
                   (value_t){.type = VALUE_TYPE_INT,
                             .as.i32 = (arg1.as.i32 + arg2.as.i32)});
        ++frame->pc;
        break;
      }
      case LOAD_LOCAL: {
        CHECK_BOUNDS(frame->pc + 1);
        uint8_t local_idx = frame->func->data[frame->pc + 1];
        fprintf(stderr, "LOAD_LOCAL idx: %d\n", local_idx);
        assert(frame->func->local_count > local_idx || "invalid local");
        frame->locals[local_idx] = pop_stack(&vm->stack);
        frame->pc += 2;
        break;
      }
      case CALL_NATIVE: {
        CHECK_BOUNDS(frame->pc + 1);
        uint8_t native_idx = frame->func->data[frame->pc + 1];
        assert(vm->native_function_count > native_idx || "invalid func");
        vm_native_func_t native_func = vm->native_functions[native_idx];
        fprintf(stderr, "CALL_NATIVE idx: %d (%s)\n", native_idx,
                native_func.name);
        native_func.func(vm->stack.values, native_func.arg_count,
                         native_func.userdata);
        frame->pc += 2;
        break;
      }
      case RETURN: {
        fprintf(stderr, "RETURN\n");
        vm_frame_t* current_frame = vm->current_frame;
        vm->current_frame = current_frame->next;
        free(current_frame);
        if (vm->current_frame == NULL)  // Nothing to return to... exit.
          return;
        break;
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