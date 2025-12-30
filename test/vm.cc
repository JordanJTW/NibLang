#include "src/vm.h"

#include <cstdint>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "test/assmbler.h"

using ::testing::_;
using ::testing::ElementsAre;

MATCHER_P(Int32Type, expected, "is an int32 vm_value_t") {
  int32_t value;
  if (!vm_as_int32(&arg, &value)) {
    *result_listener << "vm_as_int32() failed";
    return false;
  }
  return value == expected;
}

MATCHER_P(StringType, expected, "is a String vm_value_t") {
  char* str;
  size_t length = vm_as_str(&arg, &str);
  if (length == 0) {
    *result_listener << "vm_as_str() failed";
    return false;
  }
  return std::string(str, length) == expected;
}

ACTION(ReturnNullType) {
  return vm_value_t{.type = vm_value::VALUE_TYPE_NULL};
}

ACTION(FreeArgsAndReturnNullType) {
  for (const auto& arg : arg0) {
    vm_free_ref(arg);
  }
  return vm_value_t{.type = vm_value::VALUE_TYPE_NULL};
}

using MockNativeFunc =
    testing::MockFunction<vm_value_t(std::vector<vm_value_t>)>;

vm_value_t native_trampoline(vm_value_t* argv, size_t argc, void* userdata) {
  std::vector<vm_value_t> args(argv, argv + argc);
  return static_cast<MockNativeFunc*>(userdata)->Call(args);
}

TEST(VM, Init) {
  vm_t vm = new_vm(nullptr, 0, nullptr, 0, nullptr, 0);
  EXPECT_NE(vm, nullptr);
  free_vm(vm);
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

  MockNativeFunc native_func;

  vm_native_func_t native_funcs[] = {(vm_native_func_t){
      .name = "result",
      .arg_count = 1,
      .func = native_trampoline,
      .userdata = &native_func,
  }};

  EXPECT_CALL(native_func, Call(ElementsAre(Int32Type(10))))
      .WillOnce(ReturnNullType());

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

  MockNativeFunc native_func;

  vm_native_func_t native_funcs[] = {(vm_native_func_t){
      .name = "print",
      .arg_count = 1,
      .func = native_trampoline,
      .userdata = &native_func,
  }};

  EXPECT_CALL(native_func, Call(ElementsAre(StringType("hello world"))))
      .WillOnce(FreeArgsAndReturnNullType());

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

  MockNativeFunc native_func;

  vm_native_func_t native_funcs[] = {(vm_native_func_t){
      .name = "native",
      .arg_count = 1,
      .func = native_trampoline,
      .userdata = &native_func,
  }};

  ::testing::Sequence seq;
  for (int32_t expected_value = 0; expected_value < 5; ++expected_value) {
    EXPECT_CALL(native_func, Call(ElementsAre(Int32Type(expected_value))))
        .InSequence(seq)
        .WillOnce(ReturnNullType());
  }

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

  MockNativeFunc substring_func;
  MockNativeFunc print_func;

  vm_native_func_t native_funcs[] = {(vm_native_func_t){
                                         .name = "substring",
                                         .arg_count = 3,
                                         .func = native_trampoline,
                                         .userdata = &substring_func,
                                     },
                                     (vm_native_func_t){
                                         .name = "print",
                                         .arg_count = 1,
                                         .func = native_trampoline,
                                         .userdata = &print_func,
                                     }};

  EXPECT_CALL(substring_func, Call(_))
      .WillRepeatedly([](std::vector<vm_value_t> args) -> vm_value_t {
        char* string;
        size_t string_len = vm_as_str(&args[0], &string);

        int32_t start, end;
        EXPECT_TRUE(vm_as_int32(&args[1], &start) &&
                    vm_as_int32(&args[2], &end));
        EXPECT_LE(start, string_len);
        EXPECT_LE(end, string_len);
        EXPECT_LT(start, end);

        vm_free_ref(args[0]);  // Free string passed to us.

        return allocate_str_from_c_with_length(string + start, end - start);
      });

  ::testing::Sequence seq;
  for (const auto& expected_str : {"ll", "rld"}) {
    EXPECT_CALL(print_func, Call(ElementsAre(StringType(expected_str))))
        .WillOnce(FreeArgsAndReturnNullType());
  }

  vm_value_t constants[] = {
      allocate_str_from_c("hello world"),
  };

  vm_t vm = new_vm(constants, sizeof(constants) / sizeof(vm_value_t), funcs,
                   sizeof(funcs) / sizeof(vm_func_t), native_funcs,
                   sizeof(native_funcs) / sizeof(vm_native_func_t));
  vm_run(vm);
  free_vm(vm);
}