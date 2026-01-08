#include "src/promise.h"

#include <cstdint>
#include <memory>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "src/types.h"
#include "src/vm.h"
#include "test/gtest_helpers.h"

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::Return;

class PromiseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    vm_function_t functions[] = {
        {.argument_count = 1,
         .type = vm_function_t::VM_NATIVE_FUNC,
         .name = "on_fulfilled_fn",
         .as.native = {.fn = native_trampoline,
                       .userdata = &on_fullfilled_func}},
        {.argument_count = 1,
         .type = vm_function_t::VM_NATIVE_FUNC,
         .name = "on_rejected_fn",
         .as.native = {.fn = native_trampoline,
                       .userdata = &on_rejected_func}}};

    vm_ = new_vm(nullptr, 0, functions, 2);
    job_queue_ = init_job_queue();
  }

  void TearDown() override {
    free_job_queue(job_queue_);
    free_vm(vm_);
  }

  vm_t* vm_;
  vm_job_queue_t* job_queue_;

  MockNativeFunc on_fullfilled_func;
  MockNativeFunc on_rejected_func;
  vm_function_t functions[2];
};

TEST_F(PromiseTest, ResolveChain) {
  vm_value_t promise_value = allocate_promise();

  EXPECT_FALSE(run_promise_jobs(vm_, job_queue_));

  vm_value_t value = {.type = vm_value::VALUE_TYPE_INT, .as.i32 = 42};
  promise_resolve(job_queue_, promise_value, value, false);

  EXPECT_THAT(promise_value, IsFulfilledWith(Int32Type(42)));

  EXPECT_FALSE(run_promise_jobs(vm_, job_queue_));

  EXPECT_CALL(on_fullfilled_func, Call(ElementsAre(Int32Type(42))))
      .WillOnce(Return((vm_value_t){
          .type = vm_value::VALUE_TYPE_INT,
          .as.i32 = 109,
      }));

  vm_value_t then_promise =
      promise_then(job_queue_, promise_value,
                   bind_to_function(vm_, 0 /*on_fulfilled_fn*/,
                                    /*argv=*/nullptr, /*argc=*/0),
                   bind_to_function(vm_, 1 /*on_rejected_fn*/,
                                    /*argv=*/nullptr, /*argc=*/0));

  EXPECT_TRUE(run_promise_jobs(vm_, job_queue_));

  EXPECT_THAT(then_promise, IsFulfilledWith(Int32Type(109)));
}

TEST_F(PromiseTest, ResolveChainPromise) {
  vm_value_t promise_value = allocate_promise();

  EXPECT_FALSE(run_promise_jobs(vm_, job_queue_));
  vm_value_t value = {.type = vm_value::VALUE_TYPE_INT, .as.i32 = 42};
  promise_resolve(job_queue_, promise_value, value, false);

  EXPECT_THAT(promise_value, IsFulfilledWith(Int32Type(42)));

  EXPECT_FALSE(run_promise_jobs(vm_, job_queue_));

  vm_value_t inner_promise = allocate_promise();

  EXPECT_CALL(on_fullfilled_func, Call(ElementsAre(Int32Type(42))))
      .WillOnce([inner_promise](std::vector<vm_value_t> args) {
        return inner_promise;
      });

  vm_value_t then_promise =
      promise_then(job_queue_, promise_value,
                   bind_to_function(vm_, 0 /*on_fulfilled_fn*/,
                                    /*argv=*/nullptr, /*argc=*/0),
                   bind_to_function(vm_, 1 /*on_rejected_fn*/,
                                    /*argv=*/nullptr, /*argc=*/0));

  EXPECT_TRUE(run_promise_jobs(vm_, job_queue_));

  EXPECT_EQ(then_promise.as.promise->state,
            vm_promise_t::PROMISE_STATE_PENDING);

  vm_value_t inner_value = {.type = vm_value::VALUE_TYPE_INT, .as.i32 = 109};
  promise_resolve(job_queue_, inner_promise, inner_value, false);

  EXPECT_TRUE(run_promise_jobs(vm_, job_queue_));

  EXPECT_THAT(then_promise, IsFulfilledWith(Int32Type(109)));
}