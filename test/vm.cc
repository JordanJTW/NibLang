#include "src/vm.h"

#include <cstdint>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "test/assmbler.h"

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
  auto main_bytecode = Assembler()
                           .PushConst(2)
                           .PushConst(8)
                           .Call(1)
                           .StoreLocal(0)
                           .PushLocal(0)
                           .CallNative(0)
                           .Return()
                           .Build();

  auto add_bytecode =
      Assembler().PushLocal(0).PushLocal(1).Add().Return().Build();

  vm_func_t funcs[] = {
      (vm_func_t){
          .name = "main",
          .arg_count = 0,
          .local_count = 1,
          .data = main_bytecode.data(),
          .data_len = main_bytecode.size(),
      },
      (vm_func_t){
          .name = "add",
          .arg_count = 2,
          .local_count = 2,
          .data = add_bytecode.data(),
          .data_len = add_bytecode.size(),
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
  auto main_bytecode =
      Assembler().PushConstRef(0).CallNative(0).Return().Build();

  vm_value_t constants[] = {
      allocate_str_from_c("hello world"),
  };

  vm_func_t funcs[] = {
      (vm_func_t){
          .name = "main",
          .arg_count = 0,
          .local_count = 0,
          .data = main_bytecode.data(),
          .data_len = main_bytecode.size(),
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

TEST(VM, ForLoop) {
  // for (int i=0; i < 5; ++i)
  //   native_func(i);
  auto main_bytecode = Assembler()
                           .PushConst(0)
                           .StoreLocal(0)
                           .Label("loop_start")
                           .PushLocal(0)
                           .PushConst(5)
                           .Compare(OP_LESS_THAN)
                           .JumpIfFalse("exit")
                           .PushLocal(0)
                           .CallNative(0)
                           .PushLocal(0)
                           .PushConst(1)
                           .Add()
                           .StoreLocal(0)
                           .Jump("loop_start")
                           .Label("exit")
                           .Return()
                           .Build();

  vm_func_t funcs[] = {
      (vm_func_t){
          .name = "main",
          .arg_count = 0,
          .local_count = 1,
          .data = main_bytecode.data(),
          .data_len = main_bytecode.size(),
      },
  };

  testing::MockFunction<void(vm_value_t*, size_t)> native_func;

  vm_native_func_t native_funcs[] = {(vm_native_func_t){
      .name = "native",
      .arg_count = 1,
      .func = call_native_func,
      .userdata = &native_func,
  }};

  EXPECT_CALL(native_func, Call(_, _))
      .Times(5)
      .WillRepeatedly([](vm_value_t* argv, size_t argc) {
        static int32_t values[] = {0, 1, 2, 3, 4};
        static size_t idx = 0;

        EXPECT_EQ(argc, 1);
        int32_t result;
        ASSERT_TRUE(vm_as_int32(argv, &result));
        EXPECT_EQ(result, values[idx++]);
      });

  vm_t vm = new_vm(nullptr, 0, funcs, sizeof(funcs), native_funcs,
                   sizeof(native_funcs));
  vm_run(vm);
  free_vm(vm);
}