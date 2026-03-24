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
                       .userdata = &on_fulfilled_func}},
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

  MockNativeFunc on_fulfilled_func;
  MockNativeFunc on_rejected_func;
  vm_function_t functions[2];
};

TEST_F(PromiseTest, ResolveChain) {
  RC_AUTOFREE vm_value_t promise_value = allocate_promise(vm_);

  EXPECT_FALSE(run_promise_jobs(vm_, job_queue_));

  vm_value_t value = {.type = vm_value::VALUE_TYPE_INT, .as.i32 = 42};
  promise_resolve(job_queue_, promise_value, value, false);

  EXPECT_THAT(promise_value, IsFulfilledWith(Int32Type(42)));

  EXPECT_FALSE(run_promise_jobs(vm_, job_queue_));

  EXPECT_CALL(on_fulfilled_func, Call(ElementsAre(Int32Type(42))))
      .WillOnce(Return((vm_value_t){
          .type = vm_value::VALUE_TYPE_INT,
          .as.i32 = 109,
      }));

  RC_AUTOFREE vm_value_t then_promise =
      promise_then(vm_, job_queue_, promise_value,
                   bind_to_function(vm_, 0 /*on_fulfilled_fn*/,
                                    /*argv=*/nullptr, /*argc=*/0),
                   bind_to_function(vm_, 1 /*on_rejected_fn*/,
                                    /*argv=*/nullptr, /*argc=*/0));

  EXPECT_TRUE(run_promise_jobs(vm_, job_queue_));

  EXPECT_THAT(then_promise, IsFulfilledWith(Int32Type(109)));
}

TEST_F(PromiseTest, ResolveChainWithPromise) {
  RC_AUTOFREE vm_value_t promise_value = allocate_promise(vm_);

  EXPECT_FALSE(run_promise_jobs(vm_, job_queue_));
  vm_value_t value = {.type = vm_value::VALUE_TYPE_INT, .as.i32 = 42};
  promise_resolve(job_queue_, promise_value, value, false);

  EXPECT_THAT(promise_value, IsFulfilledWith(Int32Type(42)));

  EXPECT_FALSE(run_promise_jobs(vm_, job_queue_));

  RC_AUTOFREE vm_value_t inner_promise = allocate_promise(vm_);

  EXPECT_CALL(on_fulfilled_func, Call(ElementsAre(Int32Type(42))))
      .WillOnce([inner_promise](std::vector<vm_value_t> args) {
        return inner_promise;
      });

  RC_AUTOFREE vm_value_t then_promise =
      promise_then(vm_, job_queue_, promise_value,
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

TEST_F(PromiseTest, FulfillPromiseWithRejected) {
  RC_AUTOFREE vm_value_t promise_value = allocate_promise(vm_);

  EXPECT_FALSE(run_promise_jobs(vm_, job_queue_));
  vm_value_t value = {.type = vm_value::VALUE_TYPE_INT, .as.i32 = 666};
  promise_resolve(job_queue_, promise_value, value, /*is_rejected=*/true);

  EXPECT_THAT(promise_value, IsRejectedWith(Int32Type(666)));

  EXPECT_FALSE(run_promise_jobs(vm_, job_queue_));

  EXPECT_CALL(on_rejected_func, Call(ElementsAre(Int32Type(666))))
      .WillOnce([](std::vector<vm_value_t> args) {
        return args[0];  // forward error
      });

  RC_AUTOFREE vm_value_t then_promise =
      promise_then(vm_, job_queue_, promise_value,
                   bind_to_function(vm_, 0 /*on_fulfilled_fn*/,
                                    /*argv=*/nullptr, /*argc=*/0),
                   bind_to_function(vm_, 1 /*on_rejected_fn*/,
                                    /*argv=*/nullptr, /*argc=*/0));

  EXPECT_TRUE(run_promise_jobs(vm_, job_queue_));

  EXPECT_THAT(then_promise, IsFulfilledWith(Int32Type(666)));
}

TEST_F(PromiseTest, PromiseHandleCatch) {
  RC_AUTOFREE vm_value_t promise_value = allocate_promise(vm_);

  EXPECT_FALSE(run_promise_jobs(vm_, job_queue_));
  vm_value_t value = {.type = vm_value::VALUE_TYPE_INT, .as.i32 = 666};
  promise_resolve(job_queue_, promise_value, value, /*is_rejected=*/true);

  EXPECT_THAT(promise_value, IsRejectedWith(Int32Type(666)));

  EXPECT_FALSE(run_promise_jobs(vm_, job_queue_));

  EXPECT_CALL(on_fulfilled_func, Call(_)).Times(0);

  RC_AUTOFREE vm_value_t then_promise =
      promise_then(vm_, job_queue_, promise_value,
                   bind_to_function(vm_, 0 /*on_fulfilled_fn*/,
                                    /*argv=*/nullptr, /*argc=*/0),
                   (vm_value_t){.type = vm_value_t::VALUE_TYPE_NULL});

  EXPECT_TRUE(run_promise_jobs(vm_, job_queue_));
  EXPECT_THAT(then_promise, IsRejectedWith(Int32Type(666)));

  EXPECT_CALL(on_rejected_func, Call(ElementsAre(Int32Type(666))))
      .WillOnce([](const std::vector<vm_value_t>& args) {
        return (vm_value_t){.type = vm_value::VALUE_TYPE_INT, .as.i32 = 42};
      });

  RC_AUTOFREE vm_value_t catch_promise =
      promise_then(vm_, job_queue_, promise_value,
                   (vm_value_t){.type = vm_value_t::VALUE_TYPE_NULL},
                   bind_to_function(vm_, 1 /*on_rejected_fn*/,
                                    /*argv=*/nullptr, /*argc=*/0));

  EXPECT_TRUE(run_promise_jobs(vm_, job_queue_));
  EXPECT_THAT(catch_promise, IsFulfilledWith(Int32Type(42)));
}

TEST_F(PromiseTest, FulfillPromiseThrow) {
  RC_AUTOFREE vm_value_t promise_value = allocate_promise(vm_);

  EXPECT_FALSE(run_promise_jobs(vm_, job_queue_));
  vm_value_t value = {.type = vm_value::VALUE_TYPE_INT, .as.i32 = 42};
  promise_resolve(job_queue_, promise_value, value, /*is_rejected=*/false);

  EXPECT_THAT(promise_value, IsFulfilledWith(Int32Type(42)));

  EXPECT_FALSE(run_promise_jobs(vm_, job_queue_));

  EXPECT_CALL(on_fulfilled_func, Call(ElementsAre(Int32Type(42))))
      .WillOnce([vm = vm_](std::vector<vm_value_t> args) {
        return vm_throw_exception(
            vm,
            (vm_value_t){.type = vm_value_t::VALUE_TYPE_INT, .as.i32 = 666});
      });

  RC_AUTOFREE vm_value_t then_promise =
      promise_then(vm_, job_queue_, promise_value,
                   bind_to_function(vm_, 0 /*on_fulfilled_fn*/,
                                    /*argv=*/nullptr, /*argc=*/0),
                   bind_to_function(vm_, 1 /*on_rejected_fn*/,
                                    /*argv=*/nullptr, /*argc=*/0));

  EXPECT_TRUE(run_promise_jobs(vm_, job_queue_));

  EXPECT_THAT(then_promise, IsRejectedWith(Int32Type(666)));
}

TEST_F(PromiseTest, FulfillWithHeapType) {
  RC_AUTOFREE vm_value_t promise_value = allocate_promise(vm_);

  EXPECT_FALSE(run_promise_jobs(vm_, job_queue_));
  RC_AUTOFREE vm_value_t hello_string = allocate_str_from_c("hello");
  promise_resolve(job_queue_, promise_value, hello_string,
                  /*is_rejected=*/false);

  EXPECT_THAT(promise_value, IsFulfilledWith(StringType("hello")));

  EXPECT_FALSE(run_promise_jobs(vm_, job_queue_));

  vm_value_t world_string = allocate_str_from_c("world");
  EXPECT_CALL(on_fulfilled_func, Call(ElementsAre(StringType("hello"))))
      .WillOnce([&](std::vector<vm_value_t> args) { return world_string; });

  RC_AUTOFREE vm_value_t then_promise =
      promise_then(vm_, job_queue_, promise_value,
                   bind_to_function(vm_, 0 /*on_fulfilled_fn*/,
                                    /*argv=*/nullptr, /*argc=*/0),
                   bind_to_function(vm_, 1 /*on_rejected_fn*/,
                                    /*argv=*/nullptr, /*argc=*/0));

  EXPECT_TRUE(run_promise_jobs(vm_, job_queue_));

  EXPECT_THAT(then_promise, IsFulfilledWith(StringType("world")));
}