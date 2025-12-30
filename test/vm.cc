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

vm_value_t call_native_func(vm_value_t* argv, size_t argc, void* userdata) {
  auto* native_func =
      static_cast<testing::MockFunction<vm_value_t(vm_value_t*, size_t)>*>(
          userdata);
  return native_func->Call(argv, argc);
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

  testing::MockFunction<vm_value_t(vm_value_t*, size_t)> native_func;

  vm_native_func_t native_funcs[] = {(vm_native_func_t){
      .name = "result",
      .arg_count = 1,
      .func = call_native_func,
      .userdata = &native_func,
  }};

  EXPECT_CALL(native_func, Call(_, _))
      .WillOnce([](vm_value_t* argv, size_t argc) -> vm_value_t {
        EXPECT_EQ(argc, 1);
        int32_t result;
        EXPECT_TRUE(vm_as_int32(argv, &result));
        EXPECT_EQ(result, 10);
        return (vm_value_t){.type = vm_value::VALUE_TYPE_NULL};
      });

  vm_t vm =
      new_vm(nullptr, 0, funcs, sizeof(funcs) / sizeof(vm_func_t), native_funcs,
             sizeof(native_funcs) / sizeof(vm_native_func_t));
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

  testing::MockFunction<vm_value_t(vm_value_t*, size_t)> native_func;

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
        return (vm_value_t){.type = vm_value::VALUE_TYPE_NULL};
      });

  vm_t vm = new_vm(constants, sizeof(constants) / sizeof(vm_value_t), funcs,
                   sizeof(funcs) / sizeof(vm_func_t), native_funcs,
                   sizeof(native_funcs) / sizeof(vm_native_func_t));
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

  testing::MockFunction<vm_value_t(vm_value_t*, size_t)> native_func;

  vm_native_func_t native_funcs[] = {(vm_native_func_t){
      .name = "native",
      .arg_count = 1,
      .func = call_native_func,
      .userdata = &native_func,
  }};

  EXPECT_CALL(native_func, Call(_, _))
      .Times(5)
      .WillRepeatedly([](vm_value_t* argv, size_t argc) -> vm_value_t {
        static int32_t values[] = {0, 1, 2, 3, 4};
        static size_t idx = 0;

        EXPECT_EQ(argc, 1);
        int32_t result;
        EXPECT_TRUE(vm_as_int32(argv, &result));
        EXPECT_EQ(result, values[idx++]);
        return (vm_value_t){.type = vm_value::VALUE_TYPE_NULL};
      });

  vm_t vm =
      new_vm(nullptr, 0, funcs, sizeof(funcs) / sizeof(vm_func_t), native_funcs,
             sizeof(native_funcs) / sizeof(vm_native_func_t));
  vm_run(vm);
  free_vm(vm);
}

TEST(VM, RefCountString) {
  auto main_bytecode = Assembler()
                           .PushConstRef(0)
                           .PushConst(2)
                           .PushConst(4)
                           .CallNative(0)  // substring
                           .StoreLocal(0)
                           .PushLocal(0)
                           .StoreLocal(0)
                           .PushConstRef(0)
                           .PushConst(8)
                           .PushConst(11)
                           .CallNative(0)  // substring
                           .StoreLocal(1)
                           .PushLocal(0)
                           .CallNative(1)  // print
                           .PushLocal(1)
                           .CallNative(1)  // print
                           .Return()
                           .Build();

  vm_func_t funcs[] = {
      (vm_func_t){
          .name = "main",
          .arg_count = 0,
          .local_count = 2,
          .data = main_bytecode.data(),
          .data_len = main_bytecode.size(),
      },
  };

  testing::MockFunction<vm_value_t(vm_value_t*, size_t)> substring_func;
  testing::MockFunction<vm_value_t(vm_value_t*, size_t)> print_func;

  vm_native_func_t native_funcs[] = {(vm_native_func_t){
                                         .name = "substring",
                                         .arg_count = 3,
                                         .func = call_native_func,
                                         .userdata = &substring_func,
                                     },
                                     (vm_native_func_t){
                                         .name = "print",
                                         .arg_count = 1,
                                         .func = call_native_func,
                                         .userdata = &print_func,
                                     }};

  EXPECT_CALL(substring_func, Call(_, _))
      .WillRepeatedly([](vm_value_t* argv, size_t argc) -> vm_value_t {
        char* string;
        size_t string_len = vm_as_str(&argv[0], &string);

        int32_t start, end;
        EXPECT_TRUE(vm_as_int32(&argv[1], &start) &&
                    vm_as_int32(&argv[2], &end));
        EXPECT_LE(start, string_len);
        EXPECT_LE(end, string_len);
        EXPECT_LT(start, end);

        return allocate_str_from_c_with_length(string + start, end - start);
      });

  EXPECT_CALL(print_func, Call(_, _))
      .WillOnce([](vm_value_t* argv, size_t argc) -> vm_value_t {
        char* string;
        size_t string_len = vm_as_str(&argv[0], &string);

        EXPECT_EQ(std::string(string, string_len), "ll");
        vm_free_ref(argv[0]);
        return (vm_value_t){.type = vm_value::VALUE_TYPE_NULL};
      })
      .WillOnce([](vm_value_t* argv, size_t argc) -> vm_value_t {
        char* string;
        size_t string_len = vm_as_str(&argv[0], &string);

        EXPECT_EQ(std::string(string, string_len), "rld");
        vm_free_ref(argv[0]);
        return (vm_value_t){.type = vm_value::VALUE_TYPE_NULL};
      });

  vm_value_t constants[] = {
      allocate_str_from_c("hello world"),
  };

  vm_t vm = new_vm(constants, sizeof(constants) / sizeof(vm_value_t), funcs,
                   sizeof(funcs) / sizeof(vm_func_t), native_funcs,
                   sizeof(native_funcs) / sizeof(vm_native_func_t));
  vm_run(vm);
  free_vm(vm);
}