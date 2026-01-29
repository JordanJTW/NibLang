#include <assert.h>

#include "compiler/assmbler.h"
#include "src/map.h"
#include "src/vm.h"

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
    FN_PARSE_LITERAL,
    FN_IS_DIGIT,
    FN_IS_EXPONENT_CHAR,
    FN_PARSE_NUMBER,
  };

  BYTECODE_FUNCTION(is_whitespace, 1,
                    Assembler()
                        .PushLocal(0)
                        .PushInt32(' ')
                        .Compare(OP_EQUAL)
                        .PushLocal(0)
                        .PushInt32('\n')
                        .Compare(OP_EQUAL)
                        .Or()
                        .PushLocal(0)
                        .PushInt32('\r')
                        .Compare(OP_EQUAL)
                        .Or()
                        .PushLocal(0)
                        .PushInt32('\t')
                        .Compare(OP_EQUAL)
                        .Or()
                        .Return());

  BYTECODE_FUNCTION(is_digit, 1,
                    Assembler()
                        .PushLocal(0)
                        .PushInt32('0')
                        .Compare(OP_GREAT_OR_EQ)
                        .PushLocal(0)
                        .PushInt32('9')
                        .Compare(OP_LESS_OR_EQ)
                        .And()
                        .Return());

  BYTECODE_FUNCTION(is_exponent_char, 1,
                    Assembler()
                        .PushLocal(0)
                        .PushInt32('e')
                        .Compare(OP_EQUAL)
                        .PushLocal(0)
                        .PushInt32('E')
                        .Compare(OP_EQUAL)
                        .Or()
                        .Return());

  // clang-format off
  BYTECODE_FUNCTION(skip_whitespace, 2,
                    Assembler()
                        .Label("loop")
                            .PushLocal(0)  // $input
                            .PushLocal(1)  // $index
                            .DebugString("str[] from skip_ws")
                            .Call(VM_BUILTIN_STRINGS_GET, 2)
                            .StoreLocal(2)  // temp char

                            .PushLocal(2)
                            .Call(FN_IS_WHITESPACE, 1)
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
              .Call(VM_BUILTIN_STRINGS_GET, 2)
              .PushInt32('"')
              .Compare(OP_EQUAL)
              .JumpIfFalse("advance_char")
                  .DebugString("reached end of string")

                  // slice string from start_index to current index
                  .PushLocal(0)
                  .PushLocal(2)      // start_index
                  .PushLocal(1)      // current index
                  .Call(VM_BUILTIN_STRINGS_SUBSTRING, 3)
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
  //         .PushInt32(']')
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
  //             .PushInt32(',')
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
  //             .PushInt32(']')
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
          .Call(FN_SKIP_WS, 2)
          .StoreLocal(1)

          .Call(VM_BUILTIN_MAP_NEW, 0)
          .StoreLocal(2)             // local2 = map

          // check empty object
          .PushLocal(0)
          .PushLocal(1)
          .Call(VM_BUILTIN_STRINGS_GET, 2)
          .PushInt32('}')
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
              .Call(FN_SKIP_WS, 2)
              .StoreLocal(1)

              .DebugString("begin reading key")
              .PushLocal(0)
              .PushLocal(1)
              .Call(FN_PARSE_STRING, 2)
              .StoreLocal(1)
              .StoreLocal(3)         // key

              .PushLocal(0)
              .PushLocal(1)
              .Call(FN_SKIP_WS, 2)
              .StoreLocal(1)

              .Increment(1)      // skip ':'

              .DebugString("begin reading value")
              .PushLocal(0)
              .PushLocal(1)
              .Call(FN_PARSE_VALUE, 2)
              .StoreLocal(1)
              .StoreLocal(4)         // value

              .PushLocal(2)
              .PushLocal(3)
              .PushLocal(4)
              .Call(VM_BUILTIN_MAP_SET, 3)
              .DebugString("end reading value")

              .PushLocal(0)
              .PushLocal(1)
              .Call(FN_SKIP_WS, 2)
              .StoreLocal(1)

              // if next char == ',' → continue
              .DebugString("going to check for ,")
              .PushLocal(0)
              .PushLocal(1)
              .Call(VM_BUILTIN_STRINGS_GET, 2)
              .PushInt32(',')
              .Compare(OP_EQUAL)
              .DebugString("Checking for ,")
              .JumpIfFalse("check_end_object")
                  .Increment(1)
                  .Jump("object_loop")

          .Label("check_end_object")
              .DebugString("check_end_object")
              .PushLocal(0)
              .PushLocal(1)
              .Call(VM_BUILTIN_STRINGS_GET, 2)
              .PushInt32('}')
              .Compare(OP_EQUAL)
              .JumpIfFalse("object_loop")
                  .DebugString("END") 
                  .PushLocal(2)
                  .Increment(1)
                  .PushLocal(1)
                  .Return());

  BYTECODE_FUNCTION(parse_literal, 2,
      Assembler()
        // $str.startswith("true", $idx)      
        .PushLocal(0)
        .PushConstRef(1)
        .PushLocal(1)
        .Call(VM_BUILTIN_STRINGS_STARTWITH, 3)
        .JumpIfFalse("check_false")
            .PushInt32(4)     // $idx += 4;
            .PushLocal(1)
            .Add()
            .StoreLocal(1)
            .PushConstRef(4)  // result = true;
            .PushLocal(1)
            .PushConstRef(4)  // return true; (handled)
            .Return()
        .Label("check_false")
        .PushLocal(0)
        .PushConstRef(2)
        .PushLocal(1)
        .Call(VM_BUILTIN_STRINGS_STARTWITH, 3)
        .JumpIfFalse("check_null")
            .PushInt32(5)     // $idx += 5;
            .PushLocal(1)
            .Add()
            .StoreLocal(1)
            .PushConstRef(5)  // result = false;
            .PushLocal(1)
            .PushConstRef(4)  // return true; (handled)
            .Return()
        .Label("check_null")
        .PushLocal(0)
        .PushConstRef(3)
        .PushLocal(1)
        .Call(VM_BUILTIN_STRINGS_STARTWITH, 3)
        .JumpIfFalse("not_handled")
            .PushInt32(4)     // $idx += 5;
            .PushLocal(1)
            .Add()
            .StoreLocal(1)
            .PushLocal(2)     // Push empty local (NULL)
            .PushLocal(1)
            .PushConstRef(4)  // return true; (handled)
            .Return()
        .Label("not_handled")
        .PushConstRef(5)  // return false;
        .Return());

  BYTECODE_FUNCTION(parse_value, 2,
      Assembler()
          .PushLocal(0)
          .PushLocal(1)
          .Call(FN_SKIP_WS, 2)
          .StoreLocal(1)

          .PushLocal(0)
          .PushLocal(1)
          .Call(VM_BUILTIN_STRINGS_GET, 2)
          .StoreLocal(2)             // ch

          .PushLocal(2)
          .PushInt32('{')
          .Compare(OP_EQUAL)
          .JumpIfFalse("check_array")
              .PushLocal(0)
              .PushLocal(1)
              .Call(FN_PARSE_OBJECT, 2)
              .Return()

          .Label("check_array")
          .PushLocal(2)
          .PushInt32('[')
          .Compare(OP_EQUAL)
          .JumpIfFalse("parse_literal")
              // .PushLocal(0)
              // .PushLocal(1)
              // .Call(FN_PARSE_ARRAY)
              .Return()

          .Label("parse_literal")
          .PushLocal(0)
          .PushLocal(1)
          .Call(FN_PARSE_LITERAL, 2)
          .JumpIfFalse("parse_number")
              .Return()

          .Label("parse_number")
          .PushLocal(0)
          .PushLocal(1)
          .Call(FN_PARSE_NUMBER, 2)
          .JumpIfFalse("parse_string")
              .Return()


          .Label("parse_string")
              .PushLocal(0)
              .PushLocal(1)
              .Call(FN_PARSE_STRING, 2)
              .Return());

  BYTECODE_FUNCTION(parse_number, 2,
      Assembler()
      // sign = 1
      .PushInt32(1)
      .StoreLocal(2)

      // if s[i] == '-'
      .PushLocal(0)
      .PushLocal(1)
      .Call(VM_BUILTIN_STRINGS_GET, 2)
      .PushInt32('-')
      .Compare(OP_EQUAL)
      .JumpIfFalse("parse_int")
          .PushInt32(-1)
          .StoreLocal(2)
          .Increment(1)

      .Label("parse_int")
          // integer = 0
          .PushInt32(0)
          .StoreLocal(3)

          // first digit
          .PushLocal(0)
          .PushLocal(1)
          .Call(VM_BUILTIN_STRINGS_GET, 2)
          .PushInt32('0')
          .Compare(OP_EQUAL)
          .JumpIfFalse("non_zero_int")
              // exactly "0"
              .Increment(1)
              .Jump("after_int")

      .Label("non_zero_int")
          // must be 1–9
          .PushLocal(0)
          .PushLocal(1)
          .Call(VM_BUILTIN_STRINGS_GET, 2)   
          .Call(FN_IS_DIGIT, 1)  // THIS IS WRONG AND SHOULD NOT INCLUDE 0
          .JumpIfFalse("error")

      .Label("int_loop")
          .PushLocal(3)
          .PushInt32(10)
          .Multiply()

          .PushLocal(0)
          .PushLocal(1)
          .Call(VM_BUILTIN_STRINGS_GET, 2)
          .PushInt32('0')
          .Subtract()

          .Add()
          .StoreLocal(3)

          .Increment(1)

          // while digit
          .PushLocal(0)
          .PushLocal(1)
          .Call(VM_BUILTIN_STRINGS_GET, 2)
          .Call(FN_IS_DIGIT, 1)
          .Not()
          .JumpIfFalse("int_loop")

      .Label("after_int")
          // fraction?
          .PushLocal(0)
          .PushLocal(1)
          .Call(VM_BUILTIN_STRINGS_GET, 2)
          .PushInt32('.')
          .Compare(OP_EQUAL)
          .JumpIfFalse("finish")

              .Increment(1)
              .PushInt32(0)
              .StoreLocal(4)        // fraction
              .PushFloat(0.1)
              .StoreLocal(5)        // scale

          .Label("frac_loop")
              .PushLocal(0)
              .PushLocal(1)
              .Call(VM_BUILTIN_STRINGS_GET,2)
              .Call(FN_IS_DIGIT, 1)
              .JumpIfFalse("error")   // must have at least one digit

              .PushLocal(4)
              .PushLocal(0)
              .PushLocal(1)
              .Call(VM_BUILTIN_STRINGS_GET, 2)
              .PushInt32('0')
              .Subtract()
              .PushLocal(5)
              .Multiply()
              .Add()
              .StoreLocal(4)

              .PushLocal(5)
              .PushFloat(0.1)
              .Multiply()
              .StoreLocal(5)

              .Increment(1)

              .PushLocal(0)
              .PushLocal(1)
              .Call(VM_BUILTIN_STRINGS_GET, 2)
              .Call(FN_IS_DIGIT, 1)
              .Not()
              .JumpIfFalse("frac_loop")

    //   .Label("check_exp")
    //       // exponent?
    //       .PushLocal(0)
    //       .PushLocal(1)
    //       .Call(VM_BUILTIN_STRINGS_GET)
    //       .Call(FN_IS_EXPONENT_CHAR) // e or E
    //       .JumpIfFalse("finish")

    //           .Increment(1)
    //           .PushInt32(1)
    //           .StoreLocal(6) // exp sign
    //           .PushInt32(0)
    //           .StoreLocal(7) // exp value

    //           // optional sign
    //           .PushLocal(0)
    //           .PushLocal(1)
    //           .Call(VM_BUILTIN_STRINGS_GET)
    //           .PushInt32('-')
    //           .Compare(OP_EQUAL)
    //           .JumpIfFalse("exp_digits")

    //               .PushInt32(-1)
    //               .StoreLocal(6)
    //               .Increment(1)

    //       .Label("exp_digits")
    //           // must have digit
    //           .PushLocal(0)
    //           .PushLocal(1)
    //           .Call(VM_BUILTIN_STRINGS_GET)
    //           .Call(FN_IS_DIGIT)
    //           .JumpIfFalse("error")

    //       .Label("exp_loop")
    //           .PushLocal(7)
    //           .PushInt32(10)
    //           .Multiply()

    //           .PushLocal(0)
    //           .PushLocal(1)
    //           .Call(VM_BUILTIN_STRINGS_GET)
    //           .PushInt32('0')
    //           .Subtract()
    //           .Add()
    //           .StoreLocal(7)

    //           .Increment(1)

    //           .PushLocal(0)
    //           .PushLocal(1)
    //           .Call(VM_BUILTIN_STRINGS_GET)
    //           .Call(FN_IS_DIGIT)
    //           .Not()
    //           .JumpIfFalse("exp_loop")

      .Label("finish")
          // number = sign * (int + frac) * 10^(exp)
          .PushLocal(3)
          .PushLocal(4)
          .Add()
          .PushLocal(2)
          .Multiply()
        //   .PushFloat(10)
        //   .PushLocal(7)
        //   .PushLocal(6)
        //   .Multiply()
        //   .Call(VM_BUILTIN_MATH_POW)
        //   .Multiply()
          .PushLocal(1)
          .PushConstRef(4)
          .Return()
      .Label("error")
          .DebugString("ERROR")
          .PushConstRef(5)
          .Return());
  // clang-format on

  BYTECODE_FUNCTION(main, 0,
                    Assembler()
                        .PushConstRef(0)
                        .StoreLocal(0)  // local0 = input string

                        // Initialize index to 0
                        .PushInt32(0)
                        .StoreLocal(1)  // local1 = index

                        // Call parse_value(input, index)
                        .PushLocal(0)             // input string
                        .PushLocal(1)             // index
                        .Call(FN_PARSE_VALUE, 2)  // returns parsed object/array
                        .StoreLocal(1)
                        .Return()  // return parsed value
  );

  DumpByteCode(main_bytecode);

  vm_function_t parse_array = {.name = "null"};

  vm_function_t funcs[] = {
      main,         parse_value,      parse_object,  parse_array,
      parse_string, skip_whitespace,  is_whitespace, parse_literal,
      is_digit,     is_exponent_char, parse_number};

  const char* json_blob =  //"{\"sucks\": false, \"value\": 10}";
      "{"
      "\"sucks\": false, "
      "\"name\": \"ChatGPT\", "
      "\"version\": 5.0, "
      "\"bool\": true,"
      "\"exp\": 0.90210E5,"
      "\"features\": {"
      "\"null_test\": null,"
      "\"nlp\": \"advanced\", "
      "\"reasoning\": \"strong\", "
      "\"nested\": {"
      "\"level1\": {"
      "\"level2\": {"
      "\"deep\": \"value\","
      "\"number\": 12.3456"
      "}"
      "}"
      "}"
      "}, "
      "\"empty_map\": {}, "
      "\"quote_test\": \"\\\"Double quotes\\\" inside\""
      "}";

  vm_value_t constants[] = {
      allocate_str_from_c(json_blob),
      allocate_str_from_c("true"),
      allocate_str_from_c("false"),
      allocate_str_from_c("null"),
      (vm_value_t){.type = vm_value_t::VALUE_TYPE_BOOL, .as.boolean = true},
      (vm_value_t){.type = vm_value_t::VALUE_TYPE_BOOL, .as.boolean = false}};

  vm_t* vm = new_vm(constants, sizeof(constants) / sizeof(vm_value_t), funcs,
                    sizeof(funcs) / sizeof(vm_function_t));

  vm_value_t map = vm_run(vm, 0, true);

  DumpMap(map.as.map);
  vm_free_ref(&map);
  free_vm(vm);

  return 0;
}