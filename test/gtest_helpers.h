#pragma once

#include <cstddef>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "src/types.h"

// Matches vm_value_t with VALUE_TYPE_INT
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

// Matches vm_value_t with VALUE_TYPE_STRING
MATCHER_P(StringType, expected, "is a String vm_value_t") {
  char* str;
  size_t length = vm_as_str(&arg, &str);
  if (length == 0) {
    *result_listener << "vm_as_str() failed";
    return false;
  }
  return std::string(str, length) == expected;
}

// Matches a vm_value_t with VALUE_TYPE_PROMISE (and it's value)
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

static vm_value_t native_trampoline(vm_value_t* argv,
                                    size_t argc,
                                    void* userdata) {
  std::vector<vm_value_t> args(argv, argv + argc);
  return static_cast<MockNativeFunc*>(userdata)->Call(args);
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