// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "compiler/bytecode_generator.h"
#include "compiler/gtest_helpers.h"
#include "compiler/parser.h"
#include "compiler/semantic_analyzer.h"
#include "compiler/tokenizer.h"
#include "compiler/type_context.h"
#include "compiler/types.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "src/prog_types.h"
#include "src/types.h"
#include "src/vm.h"
#include "test/gtest_helpers.h"

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::i32;
using ::testing::ident;
using ::testing::Return;

namespace {

class MockErrorCollector : public ErrorCollector {
 public:
  // Mock only the Add method
  MOCK_METHOD(void, Add, (std::string_view message, Metadata meta), (override));

  // Optional: Setup a proxy so that calls to Add actually
  // store the error in the base class during a test.
  void DelegateToFake() {
    ON_CALL(*this, Add).WillByDefault([this](std::string_view m, Metadata mt) {
      this->ErrorCollector::Add(m, mt);
    });
  }
};

using MockNativeFunc =
    testing::MockFunction<vm_value_t(std::vector<vm_value_t>)>;

static vm_value_t native_trampoline(vm_value_t* argv,
                                    size_t argc,
                                    void* userdata) {
  std::vector<vm_value_t> args(argv, argv + argc);
  return static_cast<MockNativeFunc*>(userdata)->Call(args);
}

const static std::string kPreamble = R"(
extern struct Array {
  static fn new() -> Array;
  static fn init(...) -> Array;
  static fn withSize(size: i32) -> Array;

  fn push(self: Array, value: any);
  fn get(self: Array, idx: i32) -> any;
  fn set(self: Array, idx: i32, value: any) -> any;
  fn length(self: Array) -> i32;
}

extern struct Map {
  static fn new() -> Map;
  fn get(self: Map, key: any) -> any;
  fn set(self: Map, key: any, value: any) -> Void;
}

extern struct String {
  fn length(s: String) -> i32;
  fn charAt(s: String, idx: i32) -> Codepoint;
  fn substr(s: String, start: i32, end: i32) -> String;
  fn startsWith(s: String, prefix: String, idx: i32) -> bool;
}

extern fn check(...);
)";

static constexpr std::array<std::string_view, 1> kExternalFunctionNames = {
    "check"};

class GoldenTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ON_CALL(native_check_fn_, Call(_))
        .WillByDefault(FreeArgsAndReturnVoidType());
  }

  std::vector<uint8_t> BuildProgram(std::string program_text) {
    std::string full_program_text = kPreamble + program_text;
    Block root_block = Parser{full_program_text}.Parse();

    SemanticAnalyzer{type_context_, error_collector_}.Check(root_block);

    if (error_collector_.HasErrors()) {
      error_collector_.PrintAllErrors(full_program_text);
      return {};
    }

    ProgramBuilder builder;
    ByteCodeGenerator{builder, type_context_}.EmitBlock(root_block);
    builder.GetCurrentCode().Return();

    return builder.GenerateImage();
  }

  void RunProgram(const std::vector<uint8_t>& program) {
    vm_function_t kExternalFunctions[1] = {
        {.type = vm_function_t::VM_NATIVE_FUNC,
         .argument_count = 0,
         .name = "check",
         .as.native = {.fn = native_trampoline, .userdata = &native_check_fn_}},
    };

    vm_t* vm = init_vm(program.data(), program.size(), kExternalFunctions,
                       sizeof(kExternalFunctions) / sizeof(vm_function_t));
    ASSERT_NE(vm, nullptr);

    vm_run(vm, 0, false);
    free_vm(vm);
  }

  MockNativeFunc native_check_fn_;
  TypeContext type_context_{kExternalFunctionNames};
  MockErrorCollector error_collector_;
};

TEST_F(GoldenTest, AssignStatment) {
  error_collector_.DelegateToFake();

  auto program = BuildProgram(R"(
    let x = "hello";
    let y = 109;

    check(x);
    check(y);
  )");

  ASSERT_FALSE(error_collector_.HasErrors());

  {
    testing::InSequence _;
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(StringType("hello"))))
        .WillOnce(FreeArgsAndReturnVoidType());
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(Int32Type(109))))
        .WillOnce(FreeArgsAndReturnVoidType());
  }

  RunProgram(program);
}

TEST_F(GoldenTest, OptionalChainField) {
  error_collector_.DelegateToFake();

  auto program = BuildProgram(R"(
    struct Inner {
      message: String;
    }
    struct Foo {
      inner: Inner?;
    }

    let x = Foo(Inner("hello"));
    check(x.inner?.message);

    x.inner?.message = "goodbye";
    check(x.inner?.message);

    x.inner = Nil;
    check(x.inner?.message);
    check(x.inner?.message ?? "other");
    check((x.inner ?? Inner("woo")).message);
  )");

  ASSERT_FALSE(error_collector_.HasErrors());

  {
    testing::InSequence _;
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(StringType("hello"))))
        .WillOnce(FreeArgsAndReturnVoidType());
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(StringType("goodbye"))))
        .WillOnce(FreeArgsAndReturnVoidType());
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(NilType())))
        .WillOnce(FreeArgsAndReturnVoidType());
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(StringType("other"))))
        .WillOnce(FreeArgsAndReturnVoidType());
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(StringType("woo"))))
        .WillOnce(FreeArgsAndReturnVoidType());
  }

  RunProgram(program);
}

TEST_F(GoldenTest, OptionalChainFieldDeep) {
  error_collector_.DelegateToFake();

  auto program = BuildProgram(R"(
    struct Foo {
      bar: Foo?;
      message: String?;
    }

    let x = Foo(Foo(Foo(Nil, "hello"), Nil), Nil);
    check(x.bar?.bar?.message);

    x.bar?.bar?.message = "goodbye";
    check(x.bar?.bar?.message);
    check(x.bar?.bar?.bar);

    x.bar?.bar?.message = Nil;
    check(x.bar?.bar?.message ?? "other");
  )");

  ASSERT_FALSE(error_collector_.HasErrors());

  {
    testing::InSequence _;
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(StringType("hello"))))
        .WillOnce(FreeArgsAndReturnVoidType());
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(StringType("goodbye"))))
        .WillOnce(FreeArgsAndReturnVoidType());
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(NilType())))
        .WillOnce(FreeArgsAndReturnVoidType());
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(StringType("other"))))
        .WillOnce(FreeArgsAndReturnVoidType());
  }

  RunProgram(program);
}

TEST_F(GoldenTest, ArrayChain) {
  error_collector_.DelegateToFake();

  auto program = BuildProgram(R"(
    let x = Array.init(Array.init(Array.withSize(2)));

    x[0][0][1] = "Jordan";
    check(x[0][0][1]);
  )");

  ASSERT_FALSE(error_collector_.HasErrors());

  {
    testing::InSequence _;
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(StringType("Jordan"))))
        .WillOnce(FreeArgsAndReturnVoidType());
  }

  RunProgram(program);
}

TEST_F(GoldenTest, BasicFunction) {
  error_collector_.DelegateToFake();

  auto program = BuildProgram(R"(
    fn foo(value: String) {
      check("foo");
      check(value);
    }

    foo("bar");
  )");

  ASSERT_FALSE(error_collector_.HasErrors());

  {
    testing::InSequence _;
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(StringType("foo"))))
        .WillOnce(FreeArgsAndReturnVoidType());
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(StringType("bar"))))
        .WillOnce(FreeArgsAndReturnVoidType());
  }

  RunProgram(program);
}

TEST_F(GoldenTest, Closure) {
  error_collector_.DelegateToFake();

  auto program = BuildProgram(R"(
    fn foo(value: String) {
      check("foo");
      check(value);
    }

    let woo = foo;
    woo("war");
  )");

  ASSERT_FALSE(error_collector_.HasErrors());

  {
    testing::InSequence _;
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(StringType("foo"))))
        .WillOnce(FreeArgsAndReturnVoidType());
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(StringType("war"))))
        .WillOnce(FreeArgsAndReturnVoidType());
  }

  RunProgram(program);
}

TEST_F(GoldenTest, DISABLED_ClosureMethod) {
  error_collector_.DelegateToFake();

  auto program = BuildProgram(R"(
    struct Test {
      value: String;

      fn foo(self: Test, value: i32) {
        check(self.value);
        check(value);
      }
    }

    let test = Test("Safeway");
    let bound = Test.foo;

    bound(test, 109);
  )");

  ASSERT_FALSE(error_collector_.HasErrors());

  {
    testing::InSequence _;
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(StringType("Safeway"))))
        .WillOnce(FreeArgsAndReturnVoidType());
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(Int32Type(109))))
        .WillOnce(FreeArgsAndReturnVoidType());
  }

  RunProgram(program);
}

TEST_F(GoldenTest, DISABLED_TemplateStruct) {
  error_collector_.DelegateToFake();

  auto program = BuildProgram(R"(
    struct Test<T> {
      value: T;

      fn foo(self: Test<T>) -> T {
        return value;
      }
    }

    let x = Test::[i32](109);
    check(x.foo());
  )");

  ASSERT_FALSE(error_collector_.HasErrors());

  {
    testing::InSequence _;
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(Int32Type(109))))
        .WillOnce(FreeArgsAndReturnVoidType());
  }

  RunProgram(program);
}

}  // namespace