// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "src/vm.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "compiler/assembler.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "src/promise.h"
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
                           .PushInt32(2)
                           .PushInt32(8)
                           .Call(1, 2)
                           .StoreLocal(0)
                           .PushLocal(0)
                           .Call(2, 1)
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
      .WillOnce(ReturnVoidType());

  vm_t* vm = new_vm(nullptr, 0, funcs, sizeof(funcs) / sizeof(vm_function_t));
  vm_run(vm, /*entry_point_idx=*/0, false);
  free_vm(vm);
}

TEST(VM, ConstantString) {
  auto main_bytecode = Assembler().PushConstRef(0).Call(1, 1).Return().Build();

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
      .WillOnce(FreeArgsAndReturnVoidType());

  vm_t* vm = new_vm(constants, sizeof(constants) / sizeof(vm_value_t), funcs,
                    sizeof(funcs) / sizeof(vm_function_t));
  vm_run(vm, /*entry_point_idx=*/0, false);
  free_vm(vm);
}

TEST(VM, ForLoop) {
  // for (int i=0; i < 5; ++i)
  //   native_func(i);
  auto main_bytecode = Assembler()
                           .PushInt32(0)
                           .StoreLocal(0)
                           .Label("loop_start")
                           .PushLocal(0)
                           .PushInt32(5)
                           .Compare(OP_LESS_THAN)
                           .JumpIfFalse("exit")
                           .PushLocal(0)
                           .Call(1, 1)
                           .PushLocal(0)
                           .PushInt32(1)
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
        .WillOnce(ReturnVoidType());
  }

  vm_t* vm = new_vm(nullptr, 0, funcs, sizeof(funcs) / sizeof(vm_function_t));
  vm_run(vm, /*entry_point_idx=*/0, false);
  free_vm(vm);
}

TEST(VM, RefCountString) {
  auto main_bytecode = Assembler()
                           .PushConstRef(0)
                           .PushInt32(2)
                           .PushInt32(4)
                           .Call(VM_BUILTIN(4), 3)  // String.substring
                           .StoreLocal(0)
                           .PushLocal(0)
                           .StoreLocal(0)
                           .PushConstRef(0)
                           .PushInt32(8)
                           .PushInt32(11)
                           .Call(VM_BUILTIN(4), 3)  // String.substring
                           .StoreLocal(1)
                           .PushLocal(0)
                           .Call(1, 1)  // print
                           .PushLocal(1)
                           .Call(1, 1)  // print
                           .Return()
                           .Build();

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
      {.name = "print",
       .type = vm_function_t::VM_NATIVE_FUNC,
       .argument_count = 1,
       .as.native = {.fn = native_trampoline, .userdata = &print_func}}};

  ::testing::Sequence seq;
  for (const auto& expected_str : {"ll", "rld"}) {
    EXPECT_CALL(print_func, Call(ElementsAre(StringType(expected_str))))
        .WillOnce(FreeArgsAndReturnVoidType());
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
      Assembler().PushInt32(42).PushLocal(0).Add().Return().Build();

  auto main_bytecode = Assembler()
                           .Call(2 /*getPromiseNative()*/, 0)
                           .StoreLocal(0)
                           .PushLocal(0)
                           .Bind(1 /*returnValueByteCode()*/, /*argc=*/0)
                           .PushLocal(2 /*undefined*/)
                           .Call(VM_BUILTIN(3) /*Promise.then*/, 3)
                           .PushLocal(0)
                           .PushInt32(109)
                           .Call(VM_BUILTIN(1) /*Promise.fulfill*/, 2)
                           .Call(3 /*verifyResult*/, 1)
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

  vm_t* vm = new_vm(NULL, 0, funcs, sizeof(funcs) / sizeof(vm_function_t));

  vm_value_t root_promise = allocate_promise(vm);
  vm_adopt_ref(root_promise);  // Hold on a reference to `root_promise`

  EXPECT_CALL(getPromiseNative, Call(_)).WillOnce(Return(root_promise));

  vm_value_t final_promise;
  EXPECT_CALL(verifyResult, Call(_))
      .WillOnce([&final_promise](std::vector<vm_value_t> args) {
        EXPECT_EQ(args.size(), 1);
        final_promise = args[0];
        return (vm_value_t){.type = vm_value_t::VALUE_TYPE_NULL};
      });

  vm_run(vm, /*entry_point_idx=*/0, false);

  EXPECT_TRUE(run_promise_jobs(vm, vm_get_job_queue(vm)));

  EXPECT_THAT(root_promise, IsFulfilledWith(Int32Type(109)));
  EXPECT_THAT(final_promise, IsFulfilledWith(Int32Type(151)));

  vm_free_ref(&root_promise);
  // Ownership was transferred in the function call
  vm_free_ref(&final_promise);
  free_vm(vm);
}

struct TryCatchParam {
  Assembler main;
  std::vector<uint32_t> states;
};

class TryCatchTest : public ::testing::TestWithParam<TryCatchParam> {
 protected:
  void SetUp() override {
    TryCatchParam param = GetParam();

    main_bytecode_ = param.main.Build();
    vm_function_t funcs[] = {
        {.name = "main",
         .type = vm_function_t::VM_BYTECODE,
         .argument_count = 0,
         .as.bytecode =
             {
                 .data = main_bytecode_.data(),
                 .data_len = main_bytecode_.size(),
                 .local_count = 0,
             }},
        {.name = "state",
         .type = vm_function_t::VM_NATIVE_FUNC,
         .argument_count = 1,
         .as.native = {.fn = native_trampoline, .userdata = &state_func_}}};

    vm_ = new_vm(NULL, 0, funcs, sizeof(funcs) / sizeof(vm_function_t));
  }

  void TearDown() override { free_vm(vm_); }

  MockNativeFunc state_func_;
  std::vector<uint8_t> main_bytecode_;
  vm_t* vm_;
};

TEST_P(TryCatchTest, DISABLED_ValidateFlowControl) {
  ::testing::Sequence seq;
  for (uint32_t expected : GetParam().states) {
    EXPECT_CALL(state_func_, Call(ElementsAre(Int32Type(expected))))
        .WillOnce(ReturnVoidType());
  }

  vm_run(vm_, /*entry_point_idx=*/0, false);

  vm_value_t exception;
  if (vm_get_exception(vm_, &exception)) {
    state_func_.Call({exception});
  }
}

enum {
  TC_BEFORE_THROW = 0,
  TC_FIRST_EXCEPTION,
  TC_AFTER_THROW,
  TC_BEGIN_CATCH,
  TC_SECOND_EXCEPTION,
  TC_END_CATCH,
  TC_FINISH,
};

INSTANTIATE_TEST_SUITE_P(
    TryCatchTestCases,
    TryCatchTest,
    ::testing::Values(
        // Second exception from "catch" block does not retrigger initial "try"
        (TryCatchParam){.main = Assembler()
                                    .PushTry("catch_block", "finish_block")
                                    .PushInt32(TC_BEFORE_THROW)
                                    .Call(1, 1)
                                    .PushInt32(TC_FIRST_EXCEPTION)
                                    .Throw()
                                    .PushInt32(TC_AFTER_THROW)
                                    .Call(1, 1)
                                    .PopTry()
                                    .Label("catch_block")
                                    .PushInt32(TC_BEGIN_CATCH)
                                    .Call(1, 1)
                                    .Call(1, 1)  // pops exception
                                    .PushInt32(TC_SECOND_EXCEPTION)
                                    .Throw()
                                    .PushInt32(TC_END_CATCH)
                                    .Call(1, 1)
                                    .Label("finish_block")
                                    .PushInt32(TC_FINISH)
                                    .Call(1, 1)
                                    .Return(),
                        .states = {TC_BEFORE_THROW, TC_BEGIN_CATCH,
                                   TC_FIRST_EXCEPTION, TC_SECOND_EXCEPTION}},
        // Exception in "try" invokes "catch" then continues past the block
        (TryCatchParam){
            .main = Assembler()
                        .PushTry("catch_block", "finish_block")
                        .PushInt32(TC_BEFORE_THROW)
                        .Call(1, 1)
                        .PushInt32(TC_FIRST_EXCEPTION)
                        .Throw()
                        .PushInt32(TC_AFTER_THROW)
                        .Call(1, 1)
                        .PopTry()
                        .Label("catch_block")
                        .PushInt32(TC_BEGIN_CATCH)
                        .Call(1, 1)
                        .Call(1, 1)  // pops exception
                        .PushInt32(TC_END_CATCH)
                        .Call(1, 1)
                        .Label("finish_block")
                        .PushInt32(TC_FINISH)
                        .Call(1, 1)
                        .Return(),
            .states = {TC_BEFORE_THROW, TC_BEGIN_CATCH, TC_FIRST_EXCEPTION,
                       TC_END_CATCH, TC_FINISH}},
        // No "throw" in "try", calls directly to "finish_block"
        (TryCatchParam){
            .main = Assembler()
                        .PushTry("catch_block", "finish_block")
                        .PushInt32(TC_BEFORE_THROW)
                        .Call(1, 1)
                        // "throw" removed from other test cases
                        .PushInt32(TC_AFTER_THROW)
                        .Call(1, 1)
                        .PopTry()
                        .Label("catch_block")
                        .PushInt32(TC_BEGIN_CATCH)
                        .Call(1, 1)
                        .Call(1, 1)  // pops exception
                        .PushInt32(TC_END_CATCH)
                        .Call(1, 1)
                        .Label("finish_block")
                        .PushInt32(TC_FINISH)
                        .Call(1, 1)
                        .Return(),
            .states = {TC_BEFORE_THROW, TC_AFTER_THROW, TC_FINISH}}));