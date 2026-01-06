#include "vm.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/types.h"

#define LOG($fmt, ...) \
  fprintf(stderr, "[%s:%d] " $fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)

#ifdef NDEBUG
#define DEBUG_LOG($fmt, ...)
#else
// Only used in assert() statements to verify value types.
static bool is_number_type(vm_value_t value) {
  return value.type == VALUE_TYPE_INT || value.type == VALUE_TYPE_FLOAT;
}

#define DEBUG_LOG($fmt, ...) LOG($fmt, ##__VA_ARGS__)
#endif  // NDEBUG

typedef struct vm_frame vm_frame_t;

static void rc_increment(vm_value_t* value);
static void rc_decrement(vm_value_t* value);

static bool value_to_bool(vm_value_t value) {
  switch (value.type) {
    case VALUE_TYPE_NULL:
      return false;
    case VALUE_TYPE_BOOL:
      return value.as.boolean;
    case VALUE_TYPE_INT:
      return value.as.i32 != 0;
    case VALUE_TYPE_FLOAT:
      return value.as.f32 != 0.0f;
    case VALUE_TYPE_STR:
      return value.as.str->len != 0;
    default:
      return false;
  }
}

typedef enum { NUM_OP_ADD, NUM_OP_SUB, NUM_OP_MUL, NUM_OP_DIV } num_op_t;

static vm_value_t handle_number_op(num_op_t op, vm_value_t a, vm_value_t b);

uint32_t read_u32_le(const uint8_t* data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

// A fixed sized (`capacity`) stack of `vm_value_t` for the VM.
typedef struct {
  vm_value_t* values;
  size_t sp;
  size_t capacity;
} vm_stack_t;

typedef struct {
  vm_value_t* constants;
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
  vm->constants = constants;
  vm->contants_count = constants_count;

  // Ensure all constants are owned by the VM itself.
  for (size_t i = 0; i < vm->contants_count; ++i)
    rc_increment(&vm->constants[i]);

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
    rc_decrement(&_vm->constants[i]);

  assert(_vm->current_frame == NULL && "leaked frames exist");
  assert(_vm->stack.sp == 0 && "stack not empty on free");

  free(_vm->stack.values);
  free(_vm);
}

static void rc_increment(vm_value_t* value) {
  switch (value->type) {
    case VALUE_TYPE_NULL:
    case VALUE_TYPE_BOOL:
    case VALUE_TYPE_INT:
    case VALUE_TYPE_FLOAT:
      return;

    case VALUE_TYPE_MAP:
    case VALUE_TYPE_STR:
    case VALUE_TYPE_FUNCTION:
    case VALUE_TYPE_PROMISE: {
      ++(*value->as.ref);
      break;
    }
  }
}

static void rc_decrement(vm_value_t* value) {
  switch (value->type) {
    case VALUE_TYPE_NULL:
    case VALUE_TYPE_BOOL:
    case VALUE_TYPE_INT:
    case VALUE_TYPE_FLOAT:
      return;

    case VALUE_TYPE_MAP:
    case VALUE_TYPE_STR:
    case VALUE_TYPE_FUNCTION:
    case VALUE_TYPE_PROMISE: {
      assert(*value->as.ref > 0);
      --(*value->as.ref);
      if (*value->as.ref == 0)
        free(value->as.ref);
      break;
    }
  }
}

// Pushes `value` on to the stack. The stack takes ownership of `value`.
static void push_stack(vm_stack_t* stack, vm_value_t value) {
  assert(stack->sp < stack->capacity && "stack overflow");
  rc_increment(&value);
  stack->values[stack->sp++] = value;
}

// Pops the top of the stack.
// NOTE: Transfers the stack's ownership of the value to the caller!
static vm_value_t pop_stack(vm_stack_t* stack) {
  assert(stack->sp >= 1 && "stack underflow");
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
        CHECK_BOUNDS(frame->pc + 4);
        uint32_t const_idx = read_u32_le(&frame->func->data[frame->pc + 1]);
        DEBUG_LOG("OP_PUSH_CONST_REF idx: %d", const_idx);
        assert(vm->contants_count > const_idx && "invalid const");
        push_stack(&vm->stack, vm->constants[const_idx]);
        frame->pc += 5;
        break;
      }
      case OP_PUSH_CONST: {
        CHECK_BOUNDS(frame->pc + 4);
        int32_t value = read_u32_le(&frame->func->data[frame->pc + 1]);
        DEBUG_LOG("OP_PUSH_CONST value: %d", value);
        push_stack(&vm->stack,
                   (vm_value_t){.type = VALUE_TYPE_INT, .as.i32 = value});
        frame->pc += 5;
        break;
      }
      case OP_CALL: {
        CHECK_BOUNDS(frame->pc + 4);
        uint32_t func_idx = read_u32_le(&frame->func->data[frame->pc + 1]);
        assert(vm->function_count > func_idx && "invalid func");
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
        frame->pc += 5;
        break;
      }
      case OP_PUSH_LOCAL: {
        CHECK_BOUNDS(frame->pc + 4);
        uint32_t local_idx = read_u32_le(&frame->func->data[frame->pc + 1]);
        DEBUG_LOG("OP_PUSH_LOCAL idx: %d", local_idx);
        assert(frame->func->local_count > local_idx && "invalid local");
        vm_value_t value = vm->current_frame->locals[local_idx];
        push_stack(&vm->stack, value);
        frame->pc += 5;
        break;
      }
      case OP_ADD: {
        vm_value_t arg1 = pop_stack(&vm->stack);
        vm_value_t arg2 = pop_stack(&vm->stack);

        assert(is_number_type(arg1) && is_number_type(arg2) &&
               "only ints and floats are addable");

        vm_value_t result = handle_number_op(NUM_OP_ADD, arg1, arg2);
        push_stack(&vm->stack, result);
        ++frame->pc;
        break;
      }
      case OP_SUB: {
        vm_value_t arg2 = pop_stack(&vm->stack);
        vm_value_t arg1 = pop_stack(&vm->stack);

        assert(is_number_type(arg1) && is_number_type(arg2) &&
               "only ints and floats are subtractable");

        vm_value_t result = handle_number_op(NUM_OP_SUB, arg1, arg2);
        push_stack(&vm->stack, result);
        ++frame->pc;
        break;
      }
      case OP_MUL: {
        vm_value_t arg1 = pop_stack(&vm->stack);
        vm_value_t arg2 = pop_stack(&vm->stack);
        assert(is_number_type(arg1) && is_number_type(arg2) &&
               "only ints and floats are subtractable");

        vm_value_t result = handle_number_op(NUM_OP_MUL, arg1, arg2);
        push_stack(&vm->stack, result);
        ++frame->pc;
        break;
      }
      case OP_DIV: {
        vm_value_t arg2 = pop_stack(&vm->stack);
        vm_value_t arg1 = pop_stack(&vm->stack);

        assert(is_number_type(arg1) && is_number_type(arg2) &&
               "only ints and floats are subtractable");

        vm_value_t result = handle_number_op(NUM_OP_DIV, arg1, arg2);
        push_stack(&vm->stack, result);
        ++frame->pc;
        break;
      }
      case OP_AND: {
        vm_value_t arg2 = pop_stack(&vm->stack);
        vm_value_t arg1 = pop_stack(&vm->stack);

        vm_value_t result = (vm_value_t){
            .type = VALUE_TYPE_BOOL,
            .as.boolean = value_to_bool(arg1) && value_to_bool(arg2)};
        push_stack(&vm->stack, result);
        ++frame->pc;
        break;
      }
      case OP_OR: {
        vm_value_t arg2 = pop_stack(&vm->stack);
        vm_value_t arg1 = pop_stack(&vm->stack);

        vm_value_t result = (vm_value_t){
            .type = VALUE_TYPE_BOOL,
            .as.boolean = value_to_bool(arg1) || value_to_bool(arg2)};
        push_stack(&vm->stack, result);
        ++frame->pc;
        break;
      }
      case OP_NOT: {
        vm_value_t arg = pop_stack(&vm->stack);
        vm_value_t result = (vm_value_t){.type = VALUE_TYPE_BOOL,
                                         .as.boolean = !value_to_bool(arg)};
        push_stack(&vm->stack, result);
        ++frame->pc;
        break;
      }
      case OP_STORE_LOCAL: {
        CHECK_BOUNDS(frame->pc + 4);
        uint32_t local_idx = read_u32_le(&frame->func->data[frame->pc + 1]);
        DEBUG_LOG("OP_STORE_LOCAL idx: %d", local_idx);
        assert(frame->func->local_count > local_idx && "invalid local");
        rc_decrement(&frame->locals[local_idx]);
        frame->locals[local_idx] = pop_stack(&vm->stack);
        frame->pc += 5;
        break;
      }
      case OP_CALL_NATIVE: {
        CHECK_BOUNDS(frame->pc + 4);
        uint32_t native_idx = read_u32_le(&frame->func->data[frame->pc + 1]);
        assert(vm->native_function_count > native_idx && "invalid func");
        vm_native_func_t native_func = vm->native_functions[native_idx];
        DEBUG_LOG("OP_CALL_NATIVE idx: %d (%s)", native_idx, native_func.name);

        assert(vm->stack.sp >= native_func.arg_count && "stack overflow");
        vm_value_t result = native_func.func(
            vm->stack.values + vm->stack.sp - native_func.arg_count,
            native_func.arg_count, native_func.userdata);

        vm->stack.sp -= native_func.arg_count;
        if (result.type != VALUE_TYPE_NULL)
          push_stack(&vm->stack, result);
        frame->pc += 5;
        break;
      }
      case OP_RETURN: {
        DEBUG_LOG("OP_RETURN");
        vm_frame_t* current_frame = vm->current_frame;
        vm->current_frame = current_frame->next;

        for (size_t i = 0; i < current_frame->func->local_count; ++i)
          rc_decrement(&current_frame->locals[i]);

        free(current_frame);
        if (vm->current_frame == NULL)  // Nothing to return to... exit.
          return;
        break;
      }
#define EQUALITY_CASE($op, $condition)                                        \
  case $op: {                                                                 \
    vm_value_t v2 = pop_stack(&vm->stack);                                    \
    vm_value_t v1 = pop_stack(&vm->stack);                                    \
    assert(v1.type == VALUE_TYPE_INT && v2.type == VALUE_TYPE_INT &&          \
           "only i32 are comparable");                                        \
    DEBUG_LOG(#$op " %d" #$condition "%d", v1.as.i32, v2.as.i32);             \
    push_stack(&vm->stack,                                                    \
               (vm_value_t){.type = VALUE_TYPE_BOOL,                          \
                            .as.boolean = (v1.as.i32 $condition v2.as.i32)}); \
    ++frame->pc;                                                              \
    break;                                                                    \
  }

        EQUALITY_CASE(OP_LESS_THAN, <)
        EQUALITY_CASE(OP_LESS_OR_EQ, <=)
        EQUALITY_CASE(OP_EQUAL, ==)
        EQUALITY_CASE(OP_GREAT_OR_EQ, >=)
        EQUALITY_CASE(OP_GREATER_THAN, >)

#undef EQUALITY_CASE

      case OP_JUMP_IF_FALSE: {
        CHECK_BOUNDS(frame->pc + 4);
        uint32_t address = read_u32_le(&frame->func->data[frame->pc + 1]);
        DEBUG_LOG("OP_JUMP_IF_FALSE to 0x%08x", address);
        vm_value_t condition = pop_stack(&vm->stack);
        assert(condition.type == VALUE_TYPE_BOOL && "condition must be bool");
        if (condition.as.boolean == false) {
          frame->pc = address;
        } else {
          frame->pc += 5;
        }
        break;
      }

      case OP_JUMP: {
        CHECK_BOUNDS(frame->pc + 4);
        uint32_t address = read_u32_le(&frame->func->data[frame->pc + 1]);
        DEBUG_LOG("OP_JUMP to 0x%08x", address);
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

  assert(__vm->function_count >= 1 && "must have main() function");

  vm_func_t* func = &__vm->functions[0];
  vm_frame_t* frame =
      calloc(1, sizeof(vm_frame_t) + sizeof(vm_value_t) * func->local_count);

  frame->func = func;
  __vm->current_frame = frame;
  run_frame(__vm);
}

bool vm_as_int32(const vm_value_t* value, int32_t* out) {
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

size_t vm_as_str(const vm_value_t* value, char** out) {
  if (value == NULL || value->type != VALUE_TYPE_STR)
    return 0;
  *out = value->as.str->c_str;
  return value->as.str->len;
}

void vm_free_ref(vm_value_t value) {
  rc_decrement(&value);
}

static float value_to_float(vm_value_t value) {
  assert(is_number_type(value) && "value is not a number type");
  if (value.type == VALUE_TYPE_FLOAT)
    return value.as.f32;
  return (float)value.as.i32;
}

static vm_value_t handle_number_op(num_op_t op, vm_value_t a, vm_value_t b) {
  assert(is_number_type(a) && is_number_type(b) && "operands must be numbers");

  if (a.type == VALUE_TYPE_FLOAT || b.type == VALUE_TYPE_FLOAT) {
    float f1 = value_to_float(a);
    float f2 = value_to_float(b);

    float result;
    switch (op) {
      case NUM_OP_ADD:
        result = f1 + f2;
        break;
      case NUM_OP_SUB:
        result = f1 - f2;
        break;
      case NUM_OP_MUL:
        result = f1 * f2;
        break;
      case NUM_OP_DIV:
        result = f1 / f2;
        break;
    }

    return (vm_value_t){.type = VALUE_TYPE_FLOAT, .as.f32 = result};
  }

  // TODO: Revisit this design decision... Currently overflows are handled by
  // promoting to float which may be unexpected -- might prefer to throw an
  // exception instead.
  int32_t r;
  switch (op) {
    case NUM_OP_ADD:
      if (__builtin_add_overflow(a.as.i32, b.as.i32, &r))
        goto promote_to_float;
      return (vm_value_t){.type = VALUE_TYPE_INT, .as.i32 = r};

    case NUM_OP_SUB:
      if (__builtin_sub_overflow(a.as.i32, b.as.i32, &r))
        goto promote_to_float;
      return (vm_value_t){.type = VALUE_TYPE_INT, .as.i32 = r};

    case NUM_OP_MUL:
      if (__builtin_mul_overflow(a.as.i32, b.as.i32, &r))
        goto promote_to_float;
      return (vm_value_t){.type = VALUE_TYPE_INT, .as.i32 = r};

    case NUM_OP_DIV:
      assert(b.as.i32 != 0 && "division by zero");
      assert(!(a.as.i32 == INT32_MIN && b.as.i32 == -1) &&
             "signed division overflow");
      return (vm_value_t){.type = VALUE_TYPE_INT,
                          .as.i32 = a.as.i32 / b.as.i32};
  }

promote_to_float:
  return (vm_value_t){.type = VALUE_TYPE_FLOAT,
                      .as.f32 = (float)a.as.i32 + (float)b.as.i32};
}

vm_value_t vm_call_function(vm_t __vm,
                            Function* fn,
                            size_t argc,
                            vm_value_t* argv) {
  switch (fn->type) {
    case NATIVE: {
      return fn->is.native.func(argv, argc, fn->is.native.userdata);
    }
    case FUNC_IDX: {
      __vm_t* vm = (__vm_t*)__vm;
      vm_func_t* func = &vm->functions[fn->is.func_idx];
      vm_frame_t* new_frame = calloc(
          1, sizeof(vm_frame_t) + sizeof(vm_value_t) * func->local_count);

      new_frame->func = func;
      for (size_t arg_idx = 0; arg_idx < func->arg_count; ++arg_idx) {
        new_frame->locals[arg_idx] = argv[arg_idx];
      }
      new_frame->next = vm->current_frame;
      vm->current_frame = new_frame;
      run_frame(vm);
      vm_value_t result = pop_stack(&vm->stack);
      return result;
    }
  }
}