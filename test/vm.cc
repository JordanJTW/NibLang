#include "vm.h"

#include <cstdint>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using ::testing::_;

TEST(VM, Init) {
  vm_t vm = new_vm(nullptr, 0, nullptr, 0, nullptr, 0);
  EXPECT_NE(vm, nullptr);
  free_vm(vm);
}

void call_native_func(vm_value_t* argv, size_t argc, void* userdata) {
  auto* native_func =
      static_cast<testing::MockFunction<void(vm_value_t*, size_t)>*>(userdata);
  native_func->Call(argv, argc);
}

TEST(VM, CallFunc) {
  uint8_t main_bytecode[] = {
      0x01, 0x02,  // PUSH_CONST 2
      0x01, 0x08,  // PUSH_CONST 8
      0x02, 0x01,  // CALL 1
      0x07, 0x00,  // LOAD_LOCAL 0
      0x03, 0x00,  // PUSH_LOCAL 0
      0x06, 0x00,  // CALL_NATIVE 0
      0x05,        // RETURN
  };

  uint8_t add_bytecode[] = {
      0x03, 0x00,  // PUSH_LOCAL 0
      0x03, 0x01,  // PUSH_LOCAL 1
      0x04,        // ADD
      0x05,        // RETURN
  };

  vm_func_t funcs[] = {
      (vm_func_t){
          .name = "main",
          .arg_count = 0,
          .local_count = 1,
          .data = main_bytecode,
          .data_len = sizeof(main_bytecode),
      },
      (vm_func_t){
          .name = "add",
          .arg_count = 2,
          .local_count = 2,
          .data = add_bytecode,
          .data_len = sizeof(add_bytecode),
      },
  };

  testing::MockFunction<void(vm_value_t*, size_t)> native_func;

  vm_native_func_t native_funcs[] = {(vm_native_func_t){
      .name = "result",
      .arg_count = 1,
      .func = call_native_func,
      .userdata = &native_func,
  }};

  EXPECT_CALL(native_func, Call(_, _))
      .WillOnce([](vm_value_t* argv, size_t argc) {
        EXPECT_EQ(argc, 1);
        int32_t result;
        ASSERT_TRUE(vm_as_int32(argv, &result));
        EXPECT_EQ(result, 10);
      });

  vm_t vm = new_vm(nullptr, 0, funcs, sizeof(funcs), native_funcs,
                   sizeof(native_funcs));
  vm_run(vm);
  free_vm(vm);
}

TEST(VM, ConstantString) {
  uint8_t main_bytecode[] = {
      0x00, 0x00,  // PUSH_CONST_TABLE 0
      0x06, 0x00,  // CALL_NATIVE 0
      0x05,        // RETURN

  };

  vm_value_t constants[] = {
      allocate_str_from_c("hello world"),
  };

  vm_func_t funcs[] = {
      (vm_func_t){
          .name = "main",
          .arg_count = 0,
          .local_count = 0,
          .data = main_bytecode,
          .data_len = sizeof(main_bytecode),
      },
  };

  testing::MockFunction<void(vm_value_t*, size_t)> native_func;

  vm_native_func_t native_funcs[] = {(vm_native_func_t){
      .name = "print",
      .arg_count = 1,
      .func = call_native_func,
      .userdata = &native_func,
  }};

  EXPECT_CALL(native_func, Call(_, _))
      .WillOnce([](vm_value_t* argv, size_t argc) {
        EXPECT_EQ(argc, 1);
        char* str;
        size_t length = vm_as_str(argv, &str);
        EXPECT_EQ(std::string(str, length), "hello world");
      });

  vm_t vm = new_vm(constants, 1, funcs, sizeof(funcs), native_funcs,
                   sizeof(native_funcs));
  vm_run(vm);
  free_vm(vm);
}