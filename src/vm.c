#include "vm.h"

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/array.h"
#include "src/map.h"
#include "src/prog_types.h"
#include "src/promise.h"
#include "src/strings.h"
#include "src/types.h"

#define LOG($fmt, ...) \
  fprintf(stderr, "[%s:%d] " $fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)

#ifdef NDEBUG
#define DEBUG_LOG($fmt, ...)
#else
#define DEBUG_LOG($fmt, ...) LOG($fmt, ##__VA_ARGS__)
#endif  // NDEBUG

static bool is_number_type(vm_value_t value) {
  return value.type == VALUE_TYPE_INT || value.type == VALUE_TYPE_FLOAT;
}

#define VM_BUILTIN_FUNCTION_COUNT 18

typedef struct vm_frame vm_frame_t;

static size_t patch_function_idx(size_t func_idx,
                                 size_t native_functions_count) {
  bool is_builtin_function = (func_idx & VM_BUILTIN_SELECT_BITMASK) != 0;
  size_t idx = (func_idx & ~VM_BUILTIN_SELECT_BITMASK);
  assert((!is_builtin_function || idx < native_functions_count) &&
         "invoking built-in out of bounds");
  return idx + (is_builtin_function ? 0 : native_functions_count);
}

static void rc_increment(vm_value_t* value);
static void rc_decrement(vm_value_t* value);

static bool value_to_bool(vm_value_t value) {
  switch (value.type) {
    case VALUE_TYPE_VOID:
      assert(false && "VALUE_TYPE_VOID should never appear on stack");
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

// A fixed sized (`capacity`) stack of `vm_value_t` for the VM.
typedef struct {
  vm_value_t* values;
  size_t sp;
  size_t capacity;
} vm_stack_t;

typedef struct vm_t {
  vm_value_t* constants;
  size_t constants_count;
  vm_frame_t* current_frame;
  vm_stack_t stack;
  vm_job_queue_t* job_queue;
  size_t total_functions_count;
  size_t native_functions_count;
  vm_value_t exception;
  bool unhandled_exception;
  uint8_t* bytecode_data;
  vm_function_t functions[];
} vm_t;

typedef struct vm_try_catch_entry_t {
  size_t catch_block_label;
  size_t finally_block_label;
  struct vm_try_catch_entry_t* next;
} vm_try_catch_entry_t;

typedef struct vm_frame {
  struct vm_bytecode_t* code;
  size_t pc;
  vm_frame_t* next;
  vm_try_catch_entry_t* try_catch_handlers;
  vm_value_t locals[];
} vm_frame_t;

// Reads u32 (little-endian) argument at `argi` from the bytecode for current op
static uint32_t read_u32_arg(vm_frame_t* frame, size_t argi) {
  size_t idx = frame->pc + 1 + argi * 4;
  const uint8_t* data = frame->code->data + idx;
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void install_builtins(vm_t* vm);

vm_t* new_vm(const vm_value_t* constants,
             size_t constants_count,
             const vm_function_t* functions,
             size_t functions_count) {
  size_t total_functions_count = VM_BUILTIN_FUNCTION_COUNT + functions_count;
  vm_t* const vm =
      calloc(1, sizeof(vm_t) + sizeof(vm_function_t) * total_functions_count);

  install_builtins(vm);
  memcpy(vm->functions + VM_BUILTIN_FUNCTION_COUNT, functions,
         functions_count * sizeof(vm_function_t));
  vm->total_functions_count = total_functions_count;
  vm->native_functions_count = VM_BUILTIN_FUNCTION_COUNT;

  vm->constants = calloc(constants_count, sizeof(vm_value_t));
  memcpy(vm->constants, constants, constants_count * sizeof(vm_value_t));
  vm->constants_count = constants_count;

  // Ensure all constants are owned by the VM itself.
  for (size_t i = 0; i < vm->constants_count; ++i)
    rc_increment(&vm->constants[i]);

  vm->stack.capacity = 256;
  vm->stack.values = calloc(vm->stack.capacity, sizeof(vm_value_t));
  vm->job_queue = init_job_queue();
  return vm;
}

void free_vm(vm_t* vm) {
  for (size_t i = 0; i < vm->constants_count; ++i)
    rc_decrement(&vm->constants[i]);

  free(vm->constants);

  assert(vm->current_frame == NULL && "leaked frames exist");
  assert(vm->stack.sp == 0 && "stack not empty on free");

  free(vm->stack.values);
  free_job_queue(vm->job_queue);
  free(vm);
}

static void rc_increment(vm_value_t* value) {
  switch (value->type) {
    case VALUE_TYPE_VOID:
    case VALUE_TYPE_NULL:
    case VALUE_TYPE_BOOL:
    case VALUE_TYPE_INT:
    case VALUE_TYPE_FLOAT:
      return;

    case VALUE_TYPE_ARRAY:
    case VALUE_TYPE_MAP:
    case VALUE_TYPE_STR:
    case VALUE_TYPE_FUNCTION:
    case VALUE_TYPE_PROMISE: {
      ++(value->as.ref->count);
      break;
    }
  }
}

static void rc_decrement(vm_value_t* value) {
  switch (value->type) {
    case VALUE_TYPE_VOID:
    case VALUE_TYPE_NULL:
    case VALUE_TYPE_BOOL:
    case VALUE_TYPE_INT:
    case VALUE_TYPE_FLOAT:
      return;

    case VALUE_TYPE_ARRAY:
    case VALUE_TYPE_MAP:
    case VALUE_TYPE_STR:
    case VALUE_TYPE_FUNCTION:
    case VALUE_TYPE_PROMISE: {
      assert(value->as.ref->count > 0);
      --(value->as.ref->count);
      if (value->as.ref->count == 0)
        value->as.ref->deleter(value->as.ref);
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

static void exit_current_frame(vm_t* vm) {
  vm_frame_t* current_frame = vm->current_frame;
  vm->current_frame = current_frame->next;

  for (size_t i = 0; i < current_frame->code->local_count; ++i)
    rc_decrement(&current_frame->locals[i]);

  free(current_frame);
}

static bool handle_exception(vm_t* vm) {
  vm_frame_t* frame = vm->current_frame;
  if (frame == NULL)
    return false;

  for (vm_try_catch_entry_t* entry = frame->try_catch_handlers; entry != NULL;
       entry = entry->next) {
    // Begin execution of the "catch" block with the exception on the stack.
    frame->pc = entry->catch_block_label;
    push_stack(&vm->stack, vm->exception);
    vm->unhandled_exception = false;

    // Pop the current "catch" handler before dispatching to the catch block
    // or else a "throw" within the catch might retrigger it.
    frame->try_catch_handlers = entry->next;
    free(entry);

    DEBUG_LOG("Dispatch to catch at: 0x%zx", frame->pc);
    return true;
  }

  // If no "catch" block was found, this frame can be disposed of safely as
  // there are no circumstances where it is safe to resume execution within it.
  exit_current_frame(vm);
  return handle_exception(vm);
}

static void vm_invoke_closure(vm_t* vm,
                              Closure* closure,
                              vm_value_t* argv,
                              size_t argc) {
  if (closure->bound_argc == 0) {
    vm_invoke(vm, closure->fn, argv, argc);
  } else {
    size_t to_bind_argc = closure->fn->argument_count - closure->bound_argc;
    to_bind_argc = to_bind_argc > argc ? to_bind_argc : argc;
    memcpy(closure->argument_storage + closure->bound_argc, argv,
           to_bind_argc * sizeof(vm_value_t));
    // The function will consume its arguments (ownership is transferred
    // in function calls) BUT the closure must retain its copy for reuse
    for (size_t i = 0; i < closure->bound_argc; ++i)
      rc_increment(&closure->argument_storage[i]);

    vm_invoke(vm, closure->fn, closure->argument_storage,
              closure->bound_argc + to_bind_argc);
  }
}

static void run_frame(vm_t* vm, const char* name) {
#define CHECK_BOUNDS($pc)                             \
  if ($pc >= frame->code->data_len) {                 \
    DEBUG_LOG("data overflow in func: '%s'\n", name); \
    return;                                           \
  }

  while (1) {
    vm_frame_t* frame = vm->current_frame;

    if (vm->unhandled_exception) {
      if (!handle_exception(vm)) {
        char* message = NULL;
        size_t len = vm_as_str(&vm->exception, &message);
        fprintf(stderr, "Failed to exception: %.*s\n", (int)len, message);
        return;
      }
    }

    CHECK_BOUNDS(frame->pc);

    op_t op = frame->code->data[frame->pc];
    switch (op) {
      case OP_PUSH_CONST_REF: {
        CHECK_BOUNDS(frame->pc + 4);
        uint32_t const_idx = read_u32_arg(frame, 0);
        DEBUG_LOG("OP_PUSH_CONST_REF idx: %d", const_idx);
        assert(vm->constants_count > const_idx && "invalid const");
        push_stack(&vm->stack, vm->constants[const_idx]);
        frame->pc += 5;
        break;
      }
      case OP_PUSH_I32: {
        CHECK_BOUNDS(frame->pc + 4);
        int32_t value = read_u32_arg(frame, 0);
        DEBUG_LOG("OP_PUSH_I32 value: %d", value);
        push_stack(&vm->stack,
                   (vm_value_t){.type = VALUE_TYPE_INT, .as.i32 = value});
        frame->pc += 5;
        break;
      }
      case OP_PUSH_F32: {
        CHECK_BOUNDS(frame->pc + 4);
        float value = 0;
        memcpy(&value, &frame->code->data[frame->pc + 1], sizeof(float));
        DEBUG_LOG("OP_PUSH_F32 value: %f", value);
        push_stack(&vm->stack,
                   (vm_value_t){.type = VALUE_TYPE_FLOAT, .as.f32 = value});
        frame->pc += 5;
        break;
      }
      case OP_PUSH_TRUE: {
        DEBUG_LOG("OP_PUSH_TRUE");
        push_stack(&vm->stack,
                   (vm_value_t){.type = VALUE_TYPE_BOOL, .as.boolean = true});
        ++frame->pc;
        break;
      }
      case OP_PUSH_FALSE: {
        DEBUG_LOG("OP_PUSH_FALSE");
        push_stack(&vm->stack,
                   (vm_value_t){.type = VALUE_TYPE_BOOL, .as.boolean = false});
        ++frame->pc;
        break;
      }
      case OP_PUSH_NULL: {
        DEBUG_LOG("OP_PUSH_NULL");
        push_stack(&vm->stack, (vm_value_t){.type = VALUE_TYPE_NULL});
        ++frame->pc;
        break;
      }
      case OP_STACK_DUP: {
        DEBUG_LOG("OP_STACK_DUP");
        assert(vm->stack.sp >= 1 && "stack underflow");
        push_stack(&vm->stack, vm->stack.values[vm->stack.sp - 1]);
        ++frame->pc;
        break;
      }
      case OP_STACK_DEL: {
        DEBUG_LOG("OP_STACK_DEL");
        vm_value_t deleted = pop_stack(&vm->stack);
        rc_decrement(&deleted);
        ++frame->pc;
        break;
      }
      case OP_DYNAMIC_CALL: {
        CHECK_BOUNDS(frame->pc + 4);
        uint32_t argc = read_u32_arg(frame, 0);
        assert(vm->stack.sp >= argc && "stack overflow");

        DEBUG_LOG("OP_DYNAMIC_CALL argc: %d", argc);

        vm_value_t fn = pop_stack(&vm->stack);
        assert(fn.type == VALUE_TYPE_FUNCTION &&
               "call invoked on non-function");

        vm->stack.sp -= argc;
        vm_invoke_closure(vm, fn.as.fn, vm->stack.values + vm->stack.sp, argc);
        frame->pc += 5;
        break;
      }
      case OP_CALL: {
        CHECK_BOUNDS(frame->pc + 8);
        uint32_t func_idx = read_u32_arg(frame, 0);
        func_idx = patch_function_idx(func_idx, vm->native_functions_count);
        assert(vm->total_functions_count > func_idx && "invalid func idx");

        vm_function_t* fn = &vm->functions[func_idx];
        DEBUG_LOG("OP_CALL idx: %d:%d (%s)", func_idx, fn->type, fn->name);

        uint32_t argc = read_u32_arg(frame, 1);
        assert(vm->stack.sp >= argc && "stack overflow");
        vm->stack.sp -= argc;
        vm_invoke(vm, fn, vm->stack.values + vm->stack.sp, argc);
        frame->pc += 9;
        break;
      }
      case OP_PUSH_LOCAL: {
        CHECK_BOUNDS(frame->pc + 4);
        uint32_t local_idx = read_u32_arg(frame, 0);
        DEBUG_LOG("OP_PUSH_LOCAL idx: %d", local_idx);
        assert(frame->code->local_count > local_idx && "invalid local idx");
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
               "only ints and floats are multiplicative");

        vm_value_t result = handle_number_op(NUM_OP_MUL, arg1, arg2);
        push_stack(&vm->stack, result);
        ++frame->pc;
        break;
      }
      case OP_DIV: {
        vm_value_t arg2 = pop_stack(&vm->stack);
        vm_value_t arg1 = pop_stack(&vm->stack);

        assert(is_number_type(arg1) && is_number_type(arg2) &&
               "only ints and floats are divisible");

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
      case OP_INC: {
        CHECK_BOUNDS(frame->pc + 4);
        uint32_t local_idx = read_u32_arg(frame, 0);
        DEBUG_LOG("OP_INC idx: %d", local_idx);
        assert(frame->code->local_count > local_idx && "invalid local idx");
        assert(frame->locals[local_idx].type == VALUE_TYPE_INT &&
               "only i32 supported");
        ++(frame->locals[local_idx].as.i32);
        frame->pc += 5;
        break;
      }
      case OP_CONCAT: {
        vm_value_t arg2 = pop_stack(&vm->stack);
        vm_value_t arg1 = pop_stack(&vm->stack);
        DEBUG_LOG("OP_CONCAT");

        assert(arg1.type == VALUE_TYPE_STR && arg2.type == VALUE_TYPE_STR &&
               "OP_CONCAT only works with strings");

        size_t total_length = arg1.as.str->len + arg2.as.str->len;

        String* string = calloc(1, sizeof(String) + total_length + 1);
        string->len = total_length;
        memcpy(string->c_str, arg1.as.str->c_str, arg1.as.str->len);
        memcpy(string->c_str + arg1.as.str->len, arg2.as.str->c_str,
               arg2.as.str->len);

        string->c_str[total_length] = '\0';  // Add in the \0 terminator
        string->rc.deleter = &free;

        push_stack(&vm->stack,
                   (vm_value_t){.type = VALUE_TYPE_STR, .as.str = string});
        ++frame->pc;
        break;
      }
      case OP_STORE_LOCAL: {
        CHECK_BOUNDS(frame->pc + 4);
        uint32_t local_idx = read_u32_arg(frame, 0);
        DEBUG_LOG("OP_STORE_LOCAL idx: %d", local_idx);
        assert(frame->code->local_count > local_idx && "invalid local idx");
        rc_decrement(&frame->locals[local_idx]);
        frame->locals[local_idx] = pop_stack(&vm->stack);
        frame->pc += 5;
        break;
      }
      case OP_RETURN: {
        DEBUG_LOG("OP_RETURN");
        exit_current_frame(vm);
        if (vm->current_frame == NULL)  // Nothing to return to... exit.
          return;
        break;
      }
      case OP_BIND: {
        CHECK_BOUNDS(frame->pc + 8);
        uint32_t idx = read_u32_arg(frame, 0);
        uint32_t to_bind_argc = read_u32_arg(frame, 1);
        DEBUG_LOG(
            "OP_BIND idx: %d (%s) argc: %d", idx,
            vm->functions[patch_function_idx(idx, vm->native_functions_count)]
                .name,
            to_bind_argc);
        vm_value_t bound_fn = bind_to_function(
            vm, idx, vm->stack.values + vm->stack.sp - to_bind_argc,
            to_bind_argc);
        for (size_t i = 0; i < to_bind_argc; ++i)
          rc_decrement(vm->stack.values + (vm->stack.sp - to_bind_argc + i));
        vm->stack.sp -= to_bind_argc;
        push_stack(&vm->stack, bound_fn);
        frame->pc += 9;
        break;
      }
#define EQUALITY_CASE($op, $condition)                                        \
  case $op: {                                                                 \
    vm_value_t v2 = pop_stack(&vm->stack);                                    \
    vm_value_t v1 = pop_stack(&vm->stack);                                    \
    assert(v1.type == v2.type && "comparisons must be same type");            \
    switch (v1.type) {                                                        \
      case VALUE_TYPE_BOOL:                                                   \
        DEBUG_LOG(#$op " %d" #$condition "%d", v1.as.boolean, v2.as.boolean); \
        push_stack(                                                           \
            &vm->stack,                                                       \
            (vm_value_t){                                                     \
                .type = VALUE_TYPE_BOOL,                                      \
                .as.boolean = (v1.as.boolean $condition v2.as.boolean)});     \
        break;                                                                \
      case VALUE_TYPE_INT:                                                    \
        DEBUG_LOG(#$op " %d" #$condition "%d", v1.as.i32, v2.as.i32);         \
        push_stack(                                                           \
            &vm->stack,                                                       \
            (vm_value_t){.type = VALUE_TYPE_BOOL,                             \
                         .as.boolean = (v1.as.i32 $condition v2.as.i32)});    \
        break;                                                                \
      case VALUE_TYPE_FLOAT:                                                  \
        DEBUG_LOG(#$op " %f" #$condition "%f", v1.as.f32, v2.as.f32);         \
        push_stack(                                                           \
            &vm->stack,                                                       \
            (vm_value_t){.type = VALUE_TYPE_BOOL,                             \
                         .as.boolean = (v1.as.f32 $condition v2.as.f32)});    \
        break;                                                                \
      default:                                                                \
        assert("unsupported comparison type");                                \
    }                                                                         \
    ++frame->pc;                                                              \
    break;                                                                    \
  }

        EQUALITY_CASE(OP_LESS_THAN, <)
        EQUALITY_CASE(OP_LESS_OR_EQ, <=)
        EQUALITY_CASE(OP_EQUAL, ==)
        EQUALITY_CASE(OP_GREAT_OR_EQ, >=)
        EQUALITY_CASE(OP_GREATER_THAN, >)

#undef EQUALITY_CASE

      case OP_IS: {
        CHECK_BOUNDS(frame->pc + 1);
        uint8_t type = frame->code->data[frame->pc + 1];
        DEBUG_LOG("OP_IS 0x%x", type);
        vm_value_t target = pop_stack(&vm->stack);

        push_stack(&vm->stack,
                   (vm_value_t){.type = VALUE_TYPE_BOOL,
                                .as.boolean = (target.type == type)});
        vm_free_ref(&target);
        frame->pc += 2;
        break;
      }

      case OP_JUMP_IF_TRUE:
      case OP_JUMP_IF_FALSE: {
        CHECK_BOUNDS(frame->pc + 4);
        uint32_t address = read_u32_arg(frame, 0);
        DEBUG_LOG("OP_JUMP_IF_%s to 0x%04x",
                  (op == OP_JUMP_IF_TRUE ? "TRUE" : "FALSE"), address);
        vm_value_t condition = pop_stack(&vm->stack);
        assert(condition.type == VALUE_TYPE_BOOL && "condition must be bool");
        if (condition.as.boolean == (op == OP_JUMP_IF_TRUE)) {
          frame->pc = address;
        } else {
          frame->pc += 5;
        }
        break;
      }

      case OP_JUMP: {
        CHECK_BOUNDS(frame->pc + 4);
        uint32_t address = read_u32_arg(frame, 0);
        DEBUG_LOG("OP_JUMP to 0x%08x", address);
        frame->pc = address;
        break;
      }

      case OP_TRY_PUSH: {
        uint32_t catch_address = read_u32_arg(frame, 0);
        uint32_t finally_address = read_u32_arg(frame, 1);
        DEBUG_LOG("OP_TRY_PUSH catch: 0x%x finally: 0x%x", catch_address,
                  finally_address);
        vm_try_catch_entry_t* entry = calloc(1, sizeof(vm_try_catch_entry_t));
        entry->catch_block_label = catch_address;
        entry->finally_block_label = finally_address;
        entry->next = frame->try_catch_handlers;
        frame->try_catch_handlers = entry;
        frame->pc += 9;
        break;
      }

      case OP_TRY_POP: {
        DEBUG_LOG("OP_TRY_POP");
        vm_try_catch_entry_t* current_handler = frame->try_catch_handlers;
        if (current_handler != NULL) {
          frame->try_catch_handlers = current_handler->next;
          frame->pc = current_handler->finally_block_label;
          free(current_handler);
        }
        break;
      }

      case OP_THROW: {
        DEBUG_LOG("OP_THROW");
        vm->exception = pop_stack(&vm->stack);
        vm->unhandled_exception = true;
        break;
      }

      case OP_DEBUG: {
        CHECK_BOUNDS(frame->pc + 1);
        DEBUG_LOG("OP_DEBUG");
        uint8_t strlen = frame->code->data[frame->pc + 1];
        CHECK_BOUNDS(frame->pc + 2 + strlen);
        fprintf(stderr, "  %04zx => %.*s\n", frame->pc, (int)strlen,
                frame->code->data + frame->pc + 2);

        frame->pc += 2 + strlen;
        break;
      }

      default: {
        DEBUG_LOG("unknown op-code: 0x%02x", frame->code->data[frame->pc]);
        return;
      }
    }
  }
}

vm_value_t vm_run(vm_t* vm, size_t entry_point_idx, bool pop_return) {
  size_t idx = vm->native_functions_count + entry_point_idx;
  assert(idx < vm->total_functions_count && "invalid entry point idx");

  vm_function_t* fn = &vm->functions[idx];
  assert(fn->type == VM_BYTECODE && "main() should be interpreted code");
  vm_frame_t* frame = calloc(
      1, sizeof(vm_frame_t) + sizeof(vm_value_t) * fn->as.bytecode.local_count);

  frame->code = &fn->as.bytecode;
  vm->current_frame = frame;
  run_frame(vm, fn->name);

  if (pop_return)
    return pop_stack(&vm->stack);

  return (vm_value_t){.type = VALUE_TYPE_NULL};
}

bool vm_as_int32(const vm_value_t* value, int32_t* out) {
  if (value == NULL || value->type != VALUE_TYPE_INT)
    return false;
  *out = value->as.i32;
  return true;
}

vm_value_t allocate_str_from_c(const char* str) {
  return allocate_str_from_c_with_length(str, strlen(str));
}

vm_value_t allocate_str_from_c_with_length(const char* str, size_t length) {
  String* string = calloc(1, sizeof(String) + length + 1);
  string->len = length;
  memcpy(string->c_str, str, length);
  string->c_str[length] = '\0';  // Add in the \0 terminator
  string->rc.deleter = &free;
  return (vm_value_t){.type = VALUE_TYPE_STR, .as.str = string};
}

size_t vm_as_str(const vm_value_t* value, char** out) {
  if (value == NULL || value->type != VALUE_TYPE_STR)
    return 0;
  *out = value->as.str->c_str;
  return value->as.str->len;
}

void vm_adopt_ref(vm_value_t value) {
  rc_increment(&value);
}

void vm_free_ref(vm_value_t* value) {
  rc_decrement(value);
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
        DEBUG_LOG("OP_ADD %f + %f", f1, f2);
        result = f1 + f2;
        break;
      case NUM_OP_SUB:
        result = f1 - f2;
        break;
      case NUM_OP_MUL:
        DEBUG_LOG("OP_MUL %f * %f", f1, f2);
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

vm_value_t vm_call_function(vm_t* vm,
                            Closure* closure,
                            vm_value_t* argv,
                            size_t argc) {
  vm_invoke_closure(vm, closure, argv, argc);
  run_frame(vm, closure->fn->name);
  if (vm->stack.sp > 0) {
    return pop_stack(&vm->stack);
  } else {
    return (vm_value_t){.type = VALUE_TYPE_NULL};
  }
}

static vm_value_t math_pow(vm_value_t* argv, size_t argc, void* vm) {
  assert(argc == 2 && is_number_type(argv[0]) && is_number_type(argv[1]) &&
         "wrong number or type of args");

  float base = value_to_float(argv[0]);
  float exponent = value_to_float(argv[1]);

  printf("POW %f ^ %f = %f\n", base, exponent, powf(base, exponent));

  return (vm_value_t){.type = VALUE_TYPE_FLOAT, .as.f32 = powf(base, exponent)};
}

static void install_builtins(vm_t* vm) {
#define INSTALL($idx, $fn, $argc)                                           \
  static_assert($idx < VM_BUILTIN_FUNCTION_COUNT, "unable to install idx"); \
  vm->functions[$idx] = (vm_function_t) {                                   \
    .type = VM_NATIVE_FUNC, .name = #$fn, .argument_count = $argc,          \
    .as.native = {                                                          \
      .fn = $fn,                                                            \
      .userdata = vm,                                                       \
    }                                                                       \
  }

  INSTALL(0, allocate_promise, 0);
  INSTALL(1, vm_promise_fulfill, 2);
  INSTALL(2, vm_promise_reject, 2);
  INSTALL(3, vm_promise_then, 3);
  INSTALL(4, vm_strings_substring, 3);
  INSTALL(5, allocate_map, 0);
  INSTALL(6, vm_strings_get, 2);
  INSTALL(7, vm_map_set, 3);
  INSTALL(8, vm_strings_starts_with, 3);
  INSTALL(9, math_pow, 2);
  INSTALL(10, vm_array_new, 0);
  INSTALL(11, vm_array_get, 2);
  INSTALL(12, vm_array_set, 3);
  INSTALL(13, vm_string_length, 1);
  INSTALL(14, vm_array_init, 0);
  INSTALL(15, vm_array_push, 2);
  INSTALL(16, vm_map_get, 2);
  INSTALL(17, vm_array_length, 1);
}

static void free_closure(void* self) {
  Closure* fn = self;
  for (size_t i = 0; i < fn->bound_argc; ++i) {
    vm_free_ref(&fn->argument_storage[i]);
  }
  free(fn);
}

vm_value_t bind_to_function(vm_t* vm,
                            size_t idx,
                            vm_value_t* argv,
                            size_t argc) {
  vm_function_t* const fn =
      &vm->functions[patch_function_idx(idx, vm->native_functions_count)];
  assert(argc <= fn->argument_count && "more arguments than required");

  size_t storage_size = argc > 0 ? fn->argument_count : 0;
  Closure* func =
      calloc(1, sizeof(Closure) + sizeof(vm_value_t) * storage_size);
  func->fn = fn;
  func->bound_argc = argc;
  func->ref_count.deleter = &free_closure;
  memcpy(func->argument_storage, argv, argc * sizeof(vm_value_t));

  for (size_t i = 0; i < argc; ++i)
    rc_increment(&func->argument_storage[i]);
  return (vm_value_t){.type = VALUE_TYPE_FUNCTION, .as.fn = func};
}

vm_job_queue_t* vm_get_job_queue(vm_t* vm) {
  return vm->job_queue;
}

bool vm_get_exception(vm_t* vm, vm_value_t* exception) {
  if (!vm->unhandled_exception)
    return false;

  *exception = vm->exception;
  vm->unhandled_exception = false;
  return true;
}

vm_value_t vm_throw_exception(vm_t* vm, vm_value_t exception) {
  if (vm->unhandled_exception) {
    DEBUG_LOG("Throwing over top of existing exception!");
  }

  vm->exception = exception;
  vm_adopt_ref(vm->exception);
  vm->unhandled_exception = true;
  return (vm_value_t){.type = VALUE_TYPE_NULL};
}

vm_t* init_vm(const uint8_t* program,
              size_t program_size,
              const vm_function_t* native_functions,
              size_t native_functions_count) {
  if (sizeof(vm_prog_header_t) > program_size) {
    fprintf(stderr, "too small\n");
    return NULL;
  }

  vm_prog_header_t header;
  memcpy(&header, program, sizeof(vm_prog_header_t));

  if (header.magic[0] != 'I' || header.magic[1] != 'N' ||
      header.magic[2] != 'K' || header.magic[3] != '!') {
    fprintf(stderr, "wrong magic\n");
    return NULL;
  }

  size_t total_function_count = VM_BUILTIN_FUNCTION_COUNT +
                                header.function_count + native_functions_count;
  vm_t* const vm =
      calloc(1, sizeof(vm_t) + sizeof(vm_function_t) * total_function_count);

  install_builtins(vm);
  memcpy(vm->functions + VM_BUILTIN_FUNCTION_COUNT, native_functions,
         native_functions_count * sizeof(vm_function_t));

  vm->total_functions_count = total_function_count;
  vm->native_functions_count =
      VM_BUILTIN_FUNCTION_COUNT + native_functions_count;

  vm->constants = calloc(header.constant_count, sizeof(vm_value_t));
  vm->constants_count = header.constant_count;

  vm->bytecode_data = calloc(1, header.bytecode_size + header.debug_size);

  size_t offset = sizeof(vm_prog_header_t);

  size_t parsed_constants = 0;
  size_t parsed_functions = 0;
  size_t bytecode_offset = 0;
  while (offset + sizeof(vm_section_t) < program_size) {
    vm_section_t section;
    memcpy(&section, program + offset, sizeof(vm_section_t));

    offset += sizeof(vm_section_t);
    if (offset + section.size > program_size) {
      fprintf(stderr, "section too big %zu + %u > %zu\n", offset, section.size,
              program_size);
      return NULL;
    }

    switch (section.type) {
      case CONST_STR:
        vm->constants[parsed_constants++] = allocate_str_from_c_with_length(
            (char*)(program + offset), section.size);
        break;
      case FUNCTION: {
        vm_function_t* const fn =
            &vm->functions[vm->native_functions_count + (parsed_functions++)];
        fn->type = VM_BYTECODE;

        fn->argument_count = section.as.fn.argument_count;
        fn->as.bytecode.local_count = section.as.fn.local_count;
        fn->as.bytecode.data = vm->bytecode_data + bytecode_offset;
        fn->as.bytecode.data_len = section.size;
        fn->name = ((const char*)vm->bytecode_data) + header.bytecode_size +
                   section.as.fn.name_offset;
        memcpy(vm->bytecode_data + bytecode_offset, program + offset,
               section.size);
        bytecode_offset += section.size;
        break;
      }
      case DEBUG: {
        memcpy(vm->bytecode_data + header.bytecode_size, program + offset,
               section.size);
        break;
      }
    }
    offset += section.size;
  }

  if (parsed_constants != header.constant_count ||
      parsed_functions != header.function_count) {
    fprintf(stderr, "missing expected fn/const\n");
    return NULL;
  }

  // Ensure all constants are owned by the VM itself.
  for (size_t i = 0; i < vm->constants_count; ++i)
    rc_increment(&vm->constants[i]);

  vm->stack.capacity = 256;
  vm->stack.values = calloc(vm->stack.capacity, sizeof(vm_value_t));
  vm->job_queue = init_job_queue();
  return vm;
}

void vm_invoke(vm_t* vm, vm_function_t* fn, vm_value_t* argv, size_t argc) {
  assert(argc >= fn->argument_count && "not enough args");
  switch (fn->type) {
    case VM_BYTECODE: {
      vm_frame_t* new_frame =
          calloc(1, sizeof(vm_frame_t) +
                        sizeof(vm_value_t) * fn->as.bytecode.local_count);

      new_frame->code = &fn->as.bytecode;
      memcpy(&new_frame->locals, argv, argc * sizeof(vm_value_t));

      new_frame->next = vm->current_frame;
      vm->current_frame = new_frame;
      break;
    }
    case VM_NATIVE_FUNC: {
      vm_value_t result = fn->as.native.fn(argv, argc, fn->as.native.userdata);
      if (result.type != VALUE_TYPE_VOID)
        push_stack(&vm->stack, result);
      break;
    }
  }
}