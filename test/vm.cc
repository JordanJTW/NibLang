#include "src/vm.h"

#include <cstdint>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "src/promise.h"
#include "test/assmbler.h"
#include "test/gtest_helpers.h"

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::Return;

TEST(VM, Init) {
  vm_t* vm = new_vm(nullptr, 0, nullptr, 0);
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
                           .Call(2)
                           .Return()
                           .Build();

  auto add_bytecode =
      Assembler().PushLocal(0).PushLocal(1).Add().Return().Build();

  MockNativeFunc native_func;

  vm_function_t funcs[] = {
      {.name = "main",
       .type = vm_function_t::VM_BYTECODE,
       .argument_count = 0,
       .as.bytecode =
           {
               .data = main_bytecode.data(),
               .data_len = main_bytecode.size(),
               .local_count = 1,
           }},
      {.name = "add",
       .type = vm_function_t::VM_BYTECODE,
       .argument_count = 2,
       .as.bytecode =
           {
               .data = add_bytecode.data(),
               .data_len = add_bytecode.size(),
               .local_count = 2,
           }},
      {.name = "result",
       .type = vm_function_t::VM_NATIVE_FUNC,
       .argument_count = 1,
       .as.native = {.fn = native_trampoline, .userdata = &native_func}}};

  EXPECT_CALL(native_func, Call(ElementsAre(Int32Type(10))))
      .WillOnce(ReturnNullType());

  vm_t* vm = new_vm(nullptr, 0, funcs, sizeof(funcs) / sizeof(vm_function_t));
  vm_run(vm, /*entry_point_idx=*/0, false);
  free_vm(vm);
}

TEST(VM, ConstantString) {
  auto main_bytecode = Assembler().PushConstRef(0).Call(1).Return().Build();

  vm_value_t constants[] = {
      allocate_str_from_c("hello world"),
  };

  MockNativeFunc native_func;

  vm_function_t funcs[] = {
      {.name = "main",
       .type = vm_function_t::VM_BYTECODE,
       .argument_count = 0,
       .as.bytecode =
           {
               .data = main_bytecode.data(),
               .data_len = main_bytecode.size(),
               .local_count = 0,
           }},
      {.name = "print",
       .type = vm_function_t::VM_NATIVE_FUNC,
       .argument_count = 1,
       .as.native = {.fn = native_trampoline, .userdata = &native_func}}};

  EXPECT_CALL(native_func, Call(ElementsAre(StringType("hello world"))))
      .WillOnce(FreeArgsAndReturnNullType());

  vm_t* vm = new_vm(constants, sizeof(constants) / sizeof(vm_value_t), funcs,
                    sizeof(funcs) / sizeof(vm_function_t));
  vm_run(vm, /*entry_point_idx=*/0, false);
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
                           .Call(1)
                           .PushLocal(0)
                           .PushConst(1)
                           .Add()
                           .StoreLocal(0)
                           .Jump("loop_start")
                           .Label("exit")
                           .Return()
                           .Build();

  MockNativeFunc native_func;

  vm_function_t funcs[] = {
      {.name = "main",
       .type = vm_function_t::VM_BYTECODE,
       .argument_count = 0,
       .as.bytecode =
           {
               .data = main_bytecode.data(),
               .data_len = main_bytecode.size(),
               .local_count = 1,
           }},
      {.name = "native",
       .type = vm_function_t::VM_NATIVE_FUNC,
       .argument_count = 1,
       .as.native = {.fn = native_trampoline, .userdata = &native_func}}};

  ::testing::Sequence seq;
  for (int32_t expected_value = 0; expected_value < 5; ++expected_value) {
    EXPECT_CALL(native_func, Call(ElementsAre(Int32Type(expected_value))))
        .InSequence(seq)
        .WillOnce(ReturnNullType());
  }

  vm_t* vm = new_vm(nullptr, 0, funcs, sizeof(funcs) / sizeof(vm_function_t));
  vm_run(vm, /*entry_point_idx=*/0, false);
  free_vm(vm);
}

TEST(VM, RefCountString) {
  auto main_bytecode = Assembler()
                           .PushConstRef(0)
                           .PushConst(2)
                           .PushConst(4)
                           .Call(VM_BUILTIN(4))  // String.substring
                           .StoreLocal(0)
                           .PushLocal(0)
                           .StoreLocal(0)
                           .PushConstRef(0)
                           .PushConst(8)
                           .PushConst(11)
                           .Call(VM_BUILTIN(4))  // String.substring
                           .StoreLocal(1)
                           .PushLocal(0)
                           .Call(2)  // print
                           .PushLocal(1)
                           .Call(2)  // print
                           .Return()
                           .Build();

  MockNativeFunc substring_func;
  MockNativeFunc print_func;

  vm_function_t funcs[] = {
      {.name = "main",
       .type = vm_function_t::VM_BYTECODE,
       .argument_count = 0,
       .as.bytecode =
           {
               .data = main_bytecode.data(),
               .data_len = main_bytecode.size(),
               .local_count = 2,
           }},
      {.name = "substring",
       .type = vm_function_t::VM_NATIVE_FUNC,
       .argument_count = 3,
       .as.native = {.fn = native_trampoline, .userdata = &substring_func}},
      {.name = "print",
       .type = vm_function_t::VM_NATIVE_FUNC,
       .argument_count = 1,
       .as.native = {.fn = native_trampoline, .userdata = &print_func}}};

  ::testing::Sequence seq;
  for (const auto& expected_str : {"ll", "rld"}) {
    EXPECT_CALL(print_func, Call(ElementsAre(StringType(expected_str))))
        .WillOnce(FreeArgsAndReturnNullType());
  }

  vm_value_t constants[] = {
      allocate_str_from_c("hello world"),
  };

  vm_t* vm = new_vm(constants, sizeof(constants) / sizeof(vm_value_t), funcs,
                    sizeof(funcs) / sizeof(vm_function_t));
  vm_run(vm, /*entry_point_idx=*/0, false);
  free_vm(vm);
}

TEST(VM, CallBuiltInPromise) {
  // let root = getPromiseNative();
  // let final = root.then(returnValueByteCode);
  //
  // root.fulfill(109);
  // verifyResult(final);

  // Returns x + 42
  auto returnValueByteCode =
      Assembler().PushConst(42).PushLocal(0).Add().Return().Build();

  auto main_bytecode = Assembler()
                           .Call(2 /*getPromiseNative()*/)
                           .StoreLocal(0)
                           .PushLocal(0)
                           .Bind(1 /*returnValueByteCode()*/, /*argc=*/0)
                           .PushLocal(2 /*undefined*/)
                           .Call(VM_BUILTIN(3) /*Promise.then*/)
                           .PushLocal(0)
                           .PushConst(109)
                           .Call(VM_BUILTIN(1) /*Promise.fulfill*/)
                           .Call(3 /*verifyResult*/)
                           .Return()
                           .Build();

  MockNativeFunc getPromiseNative;
  MockNativeFunc verifyResult;

  vm_function_t funcs[] = {
      {.name = "main",
       .type = vm_function_t::VM_BYTECODE,
       .argument_count = 0,
       .as.bytecode =
           {
               .data = main_bytecode.data(),
               .data_len = main_bytecode.size(),
               .local_count = 3,
           }},
      {.name = "returnValueByteCode",
       .type = vm_function_t::VM_BYTECODE,
       .argument_count = 1,
       .as.bytecode =
           {
               .data = returnValueByteCode.data(),
               .data_len = returnValueByteCode.size(),
               .local_count = 1,
           }},
      {.name = "getPromiseNative",
       .type = vm_function_t::VM_NATIVE_FUNC,
       .argument_count = 0,
       .as.native = {.fn = native_trampoline, .userdata = &getPromiseNative}},
      {.name = "verifyResult",
       .type = vm_function_t::VM_NATIVE_FUNC,
       .argument_count = 1,
       .as.native = {.fn = native_trampoline, .userdata = &verifyResult}}};

  vm_value_t root_promise = allocate_promise();
  vm_adopt_ref(root_promise);  // Hold on a reference to `root_promise`

  EXPECT_CALL(getPromiseNative, Call(_)).WillOnce(Return(root_promise));

  vm_value_t final_promise;
  EXPECT_CALL(verifyResult, Call(_))
      .WillOnce([&final_promise](std::vector<vm_value_t> args) {
        EXPECT_EQ(args.size(), 1);
        final_promise = args[0];
        return (vm_value_t){.type = vm_value_t::VALUE_TYPE_NULL};
      });

  vm_t* vm = new_vm(NULL, 0, funcs, sizeof(funcs) / sizeof(vm_function_t));
  vm_run(vm, /*entry_point_idx=*/0, false);

  EXPECT_TRUE(run_promise_jobs(vm, vm_get_job_queue(vm)));

  EXPECT_THAT(root_promise, IsFulfilledWith(Int32Type(109)));
  EXPECT_THAT(final_promise, IsFulfilledWith(Int32Type(151)));

  vm_free_ref(root_promise);
  vm_free_ref(final_promise);  // Ownership was transferred in the function call
  free_vm(vm);
}
