#include "src/map.h"
#include "src/vm.h"
#include "test/assmbler.h"

#define BYTECODE_FUNCTION($name, $argc, $assembler)                     \
  Assembler::Metadata $name##_metadata;                                 \
  auto $name##_bytecode = $assembler.Build(&$name##_metadata);          \
  vm_function_t $name = {                                               \
      .name = #$name,                                                   \
      .argument_count = $argc,                                          \
      .type = vm_function_t::VM_BYTECODE,                               \
      .as.bytecode = {                                                  \
          .data = $name##_bytecode.data(),                              \
          .data_len = $name##_bytecode.size(),                          \
          .local_count = std::max($name##_metadata.max_local_index + 1, \
                                  static_cast<uint32_t>($argc))}}

int main() {
  enum {
    FN_MAIN = 0,
    FN_PARSE_VALUE,
    FN_PARSE_OBJECT,
    FN_PARSE_ARRAY,
    FN_PARSE_STRING,
    FN_SKIP_WS,
    FN_IS_WHITESPACE,
  };

  BYTECODE_FUNCTION(is_whitespace, 1,
                    Assembler()
                        .PushLocal(0)
                        .PushConst(' ')
                        .Compare(OP_EQUAL)
                        .PushLocal(0)
                        .PushConst('\n')
                        .Compare(OP_EQUAL)
                        .Or()
                        .PushLocal(0)
                        .PushConst('\r')
                        .Compare(OP_EQUAL)
                        .Or()
                        .PushLocal(0)
                        .PushConst('\t')
                        .Compare(OP_EQUAL)
                        .Or()
                        .Return());

  BYTECODE_FUNCTION(is_digit, 1,
                    Assembler()
                        .PushLocal(0)
                        .PushConst('0')
                        .Compare(OP_GREAT_OR_EQ)
                        .PushLocal(0)
                        .PushConst('9')
                        .Compare(OP_LESS_OR_EQ)
                        .And()
                        .Return());

  // clang-format off
  BYTECODE_FUNCTION(skip_whitespace, 2,
                    Assembler()
                        .Label("loop")
                            .PushLocal(0)  // $input
                            .PushLocal(1)  // $index
                            .DebugString("str[] from skip_ws")
                            .Call(VM_BUILTIN_STRINGS_GET)
                            .StoreLocal(2)  // temp char

                            .PushLocal(2)
                            .Call(FN_IS_WHITESPACE)
                            .JumpIfFalse("end")

                            .Increment(1)
                            .Jump("loop")
                        .Label("end")
                        .PushLocal(1)
                        .Return());

  BYTECODE_FUNCTION(parse_string, 2,
      Assembler()
          .DebugString("enter parse_string")
          .Increment(1)          // ++$index to skip opening '"'

          .PushLocal(1)
          .StoreLocal(2)          // start_index = index
          .DebugString("$start = $idx")

          .Label("string_loop")
              .PushLocal(0)
              .PushLocal(1)
              .Call(VM_BUILTIN_STRINGS_GET)
              .PushConst('"')
              .Compare(OP_EQUAL)
              .JumpIfFalse("advance_char")
                  .DebugString("reached end of string")

                  // slice string from start_index to current index
                  .PushLocal(0)
                  .PushLocal(2)      // start_index
                  .PushLocal(1)      // current index
                  .Call(VM_BUILTIN_STRINGS_SUBSTRING)
                  .Increment(1)  // skip closing '"'
                  .PushLocal(1)
                  .Return()

          .Label("advance_char")
              .Increment(1)
              .Jump("string_loop"));


  // BYTECODE_FUNCTION(parse_array, 2,
  //     Assembler()
  //         .PushLocal(1)
  //         .Call(FN_ADVANCE)          // skip '['
  //         .StoreLocal(1)

  //         .PushLocal(0)
  //         .PushLocal(1)
  //         .Call(FN_SKIP_WS)
  //         .StoreLocal(1)

  //         .Call(VM_BUILTIN(BI_NEW_ARRAY))
  //         .StoreLocal(2)             // local2 = array

  //         // check empty array
  //         .PushLocal(0)
  //         .PushLocal(1)
  //         .Call(VM_BUILTIN_STRINGS_GET)
  //         .PushConst(']')
  //         .Compare(OP_EQUAL)
  //         .JumpIfFalse("array_loop")
  //             .PushLocal(1)
  //             .Call(FN_ADVANCE)
  //             .Return()

  //         .Label("array_loop")
  //             .Call(FN_PARSE_VALUE)  // returns value
  //             .StoreLocal(3)

  //             .PushLocal(2)
  //             .PushLocal(3)
  //             .Call(VM_BUILTIN(BI_ARRAY_PUSH))

  //             .PushLocal(0)
  //             .PushLocal(1)
  //             .Call(FN_SKIP_WS)
  //             .StoreLocal(1)

  //             // if next char == ',' → continue
  //             .PushLocal(0)
  //             .PushLocal(1)
  //             .Call(VM_BUILTIN_STRINGS_GET)
  //             .PushConst(',')
  //             .Compare(OP_EQUAL)
  //             .JumpIfFalse("check_end_array")
  //                 .PushLocal(1)
  //                 .Call(FN_ADVANCE)
  //                 .StoreLocal(1)
  //                 .Jump("array_loop")

  //         .Label("check_end_array")
  //             .PushLocal(0)
  //             .PushLocal(1)
  //             .Call(VM_BUILTIN_STRINGS_GET)
  //             .PushConst(']')
  //             .Compare(OP_EQUAL)
  //             .JumpIfFalse("array_loop")
  //                 .PushLocal(1)
  //                 .Call(FN_ADVANCE)
  //                 .StoreLocal(1)
  //                 .PushLocal(2)
  //                 .Return());

  BYTECODE_FUNCTION(parse_object, 2,
      Assembler()
          .Increment(1)         // skip '{'
          
          .PushLocal(0)
          .PushLocal(1)
          .Call(FN_SKIP_WS)
          .StoreLocal(1)

          .Call(VM_BUILTIN_MAP_NEW)
          .StoreLocal(2)             // local2 = map

          // check empty object
          .PushLocal(0)
          .PushLocal(1)
          .Call(VM_BUILTIN_STRINGS_GET)
          .PushConst('}')
          .DebugString("check for empty object")
          .Compare(OP_EQUAL)
          .JumpIfFalse("object_loop")
              .PushLocal(2)
              .Increment(1)
              .PushLocal(1)
              .Return()

          .Label("object_loop")
              .PushLocal(0)
              .PushLocal(1)
              .Call(FN_SKIP_WS)
              .StoreLocal(1)

              .DebugString("begin reading key")
              .PushLocal(0)
              .PushLocal(1)
              .Call(FN_PARSE_STRING)
              .StoreLocal(1)
              .StoreLocal(3)         // key

              .PushLocal(0)
              .PushLocal(1)
              .Call(FN_SKIP_WS)
              .StoreLocal(1)

              .Increment(1)      // skip ':'

              .DebugString("begin reading value")
              .PushLocal(0)
              .PushLocal(1)
              .Call(FN_PARSE_VALUE)
              .StoreLocal(1)
              .StoreLocal(4)         // value

              .PushLocal(2)
              .PushLocal(3)
              .PushLocal(4)
              .Call(VM_BUILTIN_MAP_SET)
              .DebugString("end reading value")

              .PushLocal(0)
              .PushLocal(1)
              .Call(FN_SKIP_WS)
              .StoreLocal(1)

              // if next char == ',' → continue
              .DebugString("going to check for ,")
              .PushLocal(0)
              .PushLocal(1)
              .Call(VM_BUILTIN_STRINGS_GET)
              .PushConst(',')
              .Compare(OP_EQUAL)
              .DebugString("Checking for ,")
              .JumpIfFalse("check_end_object")
                  .Increment(1)
                  .Jump("object_loop")

          .Label("check_end_object")
              .DebugString("check_end_object")
              .PushLocal(0)
              .PushLocal(1)
              .Call(VM_BUILTIN_STRINGS_GET)
              .PushConst('}')
              .Compare(OP_EQUAL)
              .JumpIfFalse("object_loop")
                  .DebugString("END") 
                  .PushLocal(2)
                  .Increment(1)
                  .PushLocal(1)
                  .Return());

  BYTECODE_FUNCTION(parse_value, 2,
      Assembler()
          .PushLocal(0)
          .PushLocal(1)
          .Call(FN_SKIP_WS)
          .StoreLocal(1)

          .PushLocal(0)
          .PushLocal(1)
          .Call(VM_BUILTIN_STRINGS_GET)
          .StoreLocal(2)             // ch

          .PushLocal(2)
          .PushConst('{')
          .Compare(OP_EQUAL)
          .JumpIfFalse("check_array")
              .PushLocal(0)
              .PushLocal(1)
              .Call(FN_PARSE_OBJECT)
              .Return()

          .Label("check_array")
          .PushLocal(2)
          .PushConst('[')
          .Compare(OP_EQUAL)
          .JumpIfFalse("parse_string")
              // .PushLocal(0)
              // .PushLocal(1)
              // .Call(FN_PARSE_ARRAY)
              .Return()

          .Label("parse_string")
              .PushLocal(0)
              .PushLocal(1)
              .Call(FN_PARSE_STRING)
              .Return());
  // clang-format on

  BYTECODE_FUNCTION(main, 0,
                    Assembler()
                        .PushConstRef(0)
                        .StoreLocal(0)  // local0 = input string

                        // Initialize index to 0
                        .PushConst(0)
                        .StoreLocal(1)  // local1 = index

                        // Call parse_value(input, index)
                        .PushLocal(0)           // input string
                        .PushLocal(1)           // index
                        .Call(FN_PARSE_OBJECT)  // returns parsed object/array
                        .StoreLocal(1)
                        .Return()  // return parsed value
  );

  DumpByteCode(main_bytecode);

  vm_function_t parse_array = {.name = "null"};

  vm_function_t funcs[] = {main,         parse_value,  parse_object,
                           parse_array,  parse_string, skip_whitespace,
                           is_whitespace};

  const char* json_blob =
      "{"
      "\"name\": \"ChatGPT\", "
      "\"version\": \"5.0\", "
      "\"features\": {"
      "\"nlp\": \"advanced\", "
      "\"reasoning\": \"strong\", "
      "\"nested\": {"
      "\"level1\": {"
      "\"level2\": {"
      "\"deep\": \"value\""
      "}"
      "}"
      "}"
      "}, "
      "\"empty_map\": {}, "
      "\"quote_test\": \"\\\"Double quotes\\\" inside\""
      "}";

  vm_value_t constants[] = {
      allocate_str_from_c(json_blob),
  };

  vm_t* vm = new_vm(constants, 1, funcs, sizeof(funcs) / sizeof(vm_function_t));

  vm_value_t map = vm_run(vm, 0, true);

  DumpMap(map.as.map);

  free_vm(vm);

  return 0;
}