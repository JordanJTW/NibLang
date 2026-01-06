#include "src/promise.h"

#include <cstdint>
#include <memory>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "src/types.h"
#include "src/vm.h"

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::Return;

using MockNativeFunc =
    testing::MockFunction<vm_value_t(std::vector<vm_value_t>)>;

vm_value_t native_trampoline(vm_value_t* argv, size_t argc, void* userdata) {
  std::vector<vm_value_t> args(argv, argv + argc);
  return static_cast<MockNativeFunc*>(userdata)->Call(args);
}

MATCHER_P(Int32Type, expected, "is an int32 vm_value_t") {
  int32_t value;
  if (!vm_as_int32(&arg, &value)) {
    *result_listener << "vm_as_int32() failed";
    return false;
  }
  if (value != expected) {
    *result_listener << "value is " << value << ", expected " << expected;
    return false;
  }

  return value == expected;
}

std::ostream& operator<<(std::ostream& os, Promise::state_t state) {
  switch (state) {
    case Promise::PROMISE_STATE_PENDING:
      return os << "PENDING";
    case Promise::PROMISE_STATE_FULFILLED:
      return os << "FULFILLED";
    case Promise::PROMISE_STATE_REJECTED:
      return os << "REJECTED";
  }
}

MATCHER_P(IsFulfilledWith,
          value_matcher,
          "is a fulfilled promise with matching value") {
  if (arg.type != vm_value::VALUE_TYPE_PROMISE) {
    *result_listener << "value is not a promise";
    return false;
  }

  if (arg.as.promise->state != vm_promise_t::PROMISE_STATE_FULFILLED) {
    *result_listener << "promise state is " << arg.as.promise->state
                     << ", expected FULFILLED";
    return false;
  }

  return ExplainMatchResult(value_matcher, arg.as.promise->value,
                            result_listener);
}

std::unique_ptr<Function> make_function(vm_value_t (*func)(vm_value_t*,
                                                           size_t,
                                                           void*),
                                        MockNativeFunc* mock_function,
                                        std::string name) {
  auto fn = std::make_unique<Function>();
  fn->type = Function::NATIVE;
  fn->is.native.func = func;
  fn->is.native.userdata = mock_function;
  fn->is.native.arg_count = 1;
  fn->is.native.name = name.c_str();
  return fn;
}

class PromiseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    vm_ = new_vm(nullptr, 0, nullptr, 0, nullptr, 0);
    job_queue_ = init_job_queue();
  }

  void TearDown() override {
    free_job_queue(job_queue_);
    free_vm(vm_);
  }

  vm_t vm_;
  vm_job_queue_t* job_queue_;
};

TEST_F(PromiseTest, ResolveChain) {
  vm_value_t promise_value = allocate_promise();

  EXPECT_FALSE(run_promise_jobs(vm_, job_queue_));

  vm_value_t value = {.type = vm_value::VALUE_TYPE_INT, .as.i32 = 42};
  promise_resolve(job_queue_, promise_value, value, false);

  EXPECT_THAT(promise_value, IsFulfilledWith(Int32Type(42)));

  EXPECT_FALSE(run_promise_jobs(vm_, job_queue_));

  MockNativeFunc on_fullfilled_func;
  MockNativeFunc on_rejected_func;

  auto on_fulfilled_fn =
      make_function(native_trampoline, &on_fullfilled_func, "on_fulfilled");
  auto on_rejected_fn =
      make_function(native_trampoline, &on_rejected_func, "on_rejected");

  EXPECT_CALL(on_fullfilled_func, Call(ElementsAre(Int32Type(42))))
      .WillOnce(Return((vm_value_t){
          .type = vm_value::VALUE_TYPE_INT,
          .as.i32 = 109,
      }));

  vm_value_t then_promise =
      promise_then(job_queue_, promise_value,
                   (vm_value_t){.type = vm_value::VALUE_TYPE_FUNCTION,
                                .as.fn = on_fulfilled_fn.get()},
                   (vm_value_t){.type = vm_value::VALUE_TYPE_FUNCTION,
                                .as.fn = on_rejected_fn.get()});

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

  MockNativeFunc on_fullfilled_func;
  MockNativeFunc on_rejected_func;

  auto on_fulfilled_fn =
      make_function(native_trampoline, &on_fullfilled_func, "on_fulfilled");
  auto on_rejected_fn =
      make_function(native_trampoline, &on_rejected_func, "on_rejected");

  vm_value_t inner_promise = allocate_promise();

  EXPECT_CALL(on_fullfilled_func, Call(ElementsAre(Int32Type(42))))
      .WillOnce([inner_promise](std::vector<vm_value_t> args) {
        return inner_promise;
      });

  vm_value_t then_promise =
      promise_then(job_queue_, promise_value,
                   (vm_value_t){.type = vm_value::VALUE_TYPE_FUNCTION,
                                .as.fn = on_fulfilled_fn.get()},
                   (vm_value_t){.type = vm_value::VALUE_TYPE_FUNCTION,
                                .as.fn = on_rejected_fn.get()});

  EXPECT_TRUE(run_promise_jobs(vm_, job_queue_));

  EXPECT_EQ(then_promise.as.promise->state,
            vm_promise_t::PROMISE_STATE_PENDING);

  vm_value_t inner_value = {.type = vm_value::VALUE_TYPE_INT, .as.i32 = 109};
  promise_resolve(job_queue_, inner_promise, inner_value, false);

  EXPECT_TRUE(run_promise_jobs(vm_, job_queue_));

  EXPECT_THAT(then_promise, IsFulfilledWith(Int32Type(109)));
}