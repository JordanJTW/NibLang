// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

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

// Matches vm_value_t with VALUE_TYPE_NULL
MATCHER(NilType, "is a Nil vm_value_t") {
  return arg.type == vm_value_t::VALUE_TYPE_NULL;
}

MATCHER_P(HasType, expected, "is a vm_value_t with type") {
  return arg.type == expected;
}

MATCHER_P2(IsPromiseWithStateAndValue,
           expceted_state,
           value_matcher,
           "is promise with state and matching value") {
  if (arg.type != vm_value::VALUE_TYPE_PROMISE) {
    *result_listener << "value is not a promise";
    return false;
  }

  if (arg.as.promise->state != expceted_state) {
    *result_listener << "promise state is " << arg.as.promise->state
                     << ", expected " << expceted_state;
    return false;
  }

  return ExplainMatchResult(value_matcher, arg.as.promise->value,
                            result_listener);
}

// Matches VALUE_TYPE_PROMISE in FULFILLED state with the given value
MATCHER_P(IsFulfilledWith,
          value_matcher,
          "is a fulfilled promise with matching value") {
  return ExplainMatchResult(
      IsPromiseWithStateAndValue(vm_promise_t::PROMISE_STATE_FULFILLED,
                                 value_matcher),
      arg, result_listener);
}

// Matches VALUE_TYPE_PROMISE in REJECTED state with the given value
MATCHER_P(IsRejectedWith,
          value_matcher,
          "is a rejected promise with matching value") {
  return ExplainMatchResult(
      IsPromiseWithStateAndValue(vm_promise_t::PROMISE_STATE_REJECTED,
                                 value_matcher),
      arg, result_listener);
}

ACTION(ReturnVoidType) {
  return vm_value_t{.type = vm_value::VALUE_TYPE_VOID};
}

ACTION(FreeArgsAndReturnVoidType) {
  for (const auto& arg : arg0) {
    vm_free_ref(const_cast<vm_value_t*>(&arg));
  }
  return vm_value_t{.type = vm_value::VALUE_TYPE_VOID};
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

void PrintTo(const vm_value_t& value, ::std::ostream* os) {
  switch (value.type) {
    case vm_value_t::VALUE_TYPE_INT:
      *os << value.as.i32 << "i";
      break;
    case vm_value_t::VALUE_TYPE_FLOAT:
      *os << value.as.f32 << "f";
      break;
    case vm_value_t::VALUE_TYPE_BOOL:
      *os << (value.as.boolean ? "True" : "False");
      break;
    case vm_value_t::VALUE_TYPE_STR:
      *os << "\"" << value.as.str->c_str << "\"";
      break;
    case vm_value_t::VALUE_TYPE_NULL:
      *os << "Nil";
      break;
    default:
      *os << "UnknownType(" << value.type << ")";
      break;
  }
}