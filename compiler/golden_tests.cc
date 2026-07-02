// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "compiler/assembler.h"
#include "compiler/bytecode_generator.h"
#include "compiler/error_collector.h"
#include "compiler/gtest_helpers.h"
#include "compiler/mock_error_collector.h"
#include "compiler/parser.h"
#include "compiler/program_builder.h"
#include "compiler/semantic_analyzer.h"
#include "compiler/tokenizer.h"
#include "compiler/type_context.h"
#include "compiler/types.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "src/types.h"
#include "src/vm.h"
#include "test/gtest_helpers.h"

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::i32;
using ::testing::ident;
using ::testing::Return;

namespace {

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
    std::string full_program_text =
        kPreamble + "\nfn main() {\n" + program_text + "\n}";
    Block root_block = Parser{full_program_text, error_collector_}.Parse();

    SemanticAnalyzer::FunctionContext context = {{}, TypeRegistry::Any};
    SemanticAnalyzer{type_context_, scope_manager_, error_collector_,
                     type_registry_}
        .Check(root_block, context);

    if (error_collector_.HasErrors()) {
      error_collector_.PrintAllErrors(":text:", full_program_text);
      return {};
    }

    std::vector<ByteCodeGenerator::FunctionObject> function_objects;
    std::vector<const FunctionSymbol*> external_functions;

    ConstantPool constant_pool;
    for (const auto& [id, symbol] : type_registry_.symbol_table()) {
      if (const auto* fn_symbol = std::get_if<FunctionSymbol>(&symbol)) {
        if (fn_symbol->instances.empty())
          continue;

        if (!fn_symbol->IsExtern()) {
          CHECK(fn_symbol->declaration.body);

          std::vector<SymbolId> called_symbols;
          function_objects.push_back(
              ByteCodeGenerator{scope_manager_, constant_pool}.Build(
                  *fn_symbol, called_symbols));
        } else {
          external_functions.push_back(fn_symbol);
        }
      }
    }

    ProgramBuilder builder{constant_pool, kExternalFunctionNames};
    return builder.GenerateImage(function_objects, external_functions);
  }

  void RunProgram(const std::vector<uint8_t>& program) {
    vm_function_t kExternalFunctions[1] = {
        {.type = vm_function_t::VM_NATIVE_FUNC,
         .argument_count = 0,
         .name = "check",
         .as = {.native = {native_trampoline, &native_check_fn_}}}};

    vm_t* vm = init_vm(program.data(), program.size(), kExternalFunctions,
                       sizeof(kExternalFunctions) / sizeof(vm_function_t));
    ASSERT_NE(vm, nullptr);

    vm_run(vm, 0, false);
    free_vm(vm);
  }

  MockNativeFunc native_check_fn_;
  ScopeManager scope_manager_;
  TypeRegistry type_registry_{scope_manager_};
  MockErrorCollector error_collector_;
  TypeContext type_context_{scope_manager_, type_registry_, error_collector_};
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

TEST_F(GoldenTest, DISABLED_ArrayChain) {
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

TEST_F(GoldenTest, DISABLED_Closure) {
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

TEST_F(GoldenTest, Captures) {
  error_collector_.DelegateToFake();

  auto program = BuildProgram(R"(
    fn call_closure(arg: fn(i32), x: i32) {
      arg(x);
    }

    let x = 23;
    let foo = fn (i: i32) {
      check(x + i);
    };

    call_closure(foo, 5);
    call_closure(foo, 32);
  )");

  ASSERT_FALSE(error_collector_.HasErrors());

  {
    testing::InSequence _;
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(Int32Type(28))))
        .WillOnce(FreeArgsAndReturnVoidType());
    EXPECT_CALL(native_check_fn_, Call(ElementsAre(Int32Type(55))))
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

TEST_F(GoldenTest, TemplateStruct) {
  error_collector_.DelegateToFake();

  auto program = BuildProgram(R"(
    struct Test[T] {
      value: T;

      fn foo(self: Test[T]) -> T {
        return self.value;
      }
    }

    let x = Test.of(i32)(109);
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