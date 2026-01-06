#include <cstdint>
#include <iostream>

#include "src/types.h"
#include "src/vm.h"

vm_value_t native_print_number(vm_value_t* argv,
                               size_t argc,
                               void* userdata) {
  int32_t value = 0;
  vm_as_int32(&argv[0], &value);
  // std::cout << value << std::endl;
  return (vm_value_t){.type = vm_value::VALUE_TYPE_NULL};
}

int main() {
  uint8_t bytecode[] = {
      0x01, 0x00, 0x00, 0x00, 0x00,  // 0000: OP_PUSH_CONST 0
      0x03, 0x00, 0x00, 0x00, 0x00,  // 0005: OP_STORE_LOCAL 0
      0x02, 0x00, 0x00, 0x00, 0x00,  // 000a: OP_PUSH_LOCAL 0
      0x01, 0x80, 0xf0, 0xfa, 0x02,  // 000f: OP_PUSH_CONST 50000000
      0x0e,                          // 0014: OP_LESS_THAN
      0x13, 0x39, 0x00, 0x00, 0x00,  // 0015: OP_JUMP_IF_FALSE 0x39
      0x02, 0x00, 0x00, 0x00, 0x00,  // 001a: OP_PUSH_LOCAL 0
      0x06, 0x00, 0x00, 0x00, 0x00,  // 001f: OP_CALL_NATIVE 0
      0x02, 0x00, 0x00, 0x00, 0x00,  // 0024: OP_PUSH_LOCAL 0
      0x01, 0x01, 0x00, 0x00, 0x00,  // 0029: OP_PUSH_CONST 1
      0x07,                          // 002e: OP_ADD
      0x03, 0x00, 0x00, 0x00, 0x00,  // 002f: OP_STORE_LOCAL 0
      0x14, 0x0a, 0x00, 0x00, 0x00,  // 0034: OP_JUMP 0xa
      0x05,                          // 0039: OP_RETURN
  };

  vm_func_t funcs[] = {
      (vm_func_t){
          .name = "main",
          .arg_count = 0,
          .local_count = 2,
          .data = bytecode,
          .data_len = sizeof(bytecode),
      },
  };

  vm_native_func_t native_funcs[] = {(vm_native_func_t){
      .name = "print_number",
      .arg_count = 1,
      .func = native_print_number,
      .userdata = nullptr,
  }};

  vm_t vm =
      new_vm(NULL, 0, funcs, sizeof(funcs) / sizeof(vm_func_t), native_funcs,
             sizeof(native_funcs) / sizeof(vm_native_func_t));
  vm_run(vm);
  free_vm(vm);
  return 0;
}