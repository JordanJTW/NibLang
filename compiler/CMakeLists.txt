add_library(assembler assembler.cc)
target_include_directories(assembler PRIVATE ${PROJECT_SOURCE_DIR})

add_library(bytecode_generator bytecode_generator.cc)
target_link_libraries(bytecode_generator PRIVATE assembler parser printer program_builder semantic_analyzer tokenizer type_context)
target_include_directories(bytecode_generator PRIVATE ${PROJECT_SOURCE_DIR})

add_library(error_collector error_collector.cc)
target_link_libraries(error_collector PRIVATE types)
target_include_directories(error_collector PRIVATE ${PROJECT_SOURCE_DIR})

add_library(logging logging.cc)
target_include_directories(logging PRIVATE ${PROJECT_SOURCE_DIR})

add_library(printer printer.cc)
target_include_directories(printer PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(printer PRIVATE type_context types)

add_library(parser parser.cc)
target_include_directories(parser PRIVATE ${PROJECT_SOURCE_DIR})

add_library(program_builder program_builder.cc)
target_include_directories(program_builder PRIVATE ${PROJECT_SOURCE_DIR})

add_library(semantic_analyzer semantic_analyzer.cc)
target_include_directories(semantic_analyzer PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(semantic_analyzer PRIVATE error_collector logging printer type_context)

add_library(tokenizer tokenizer.cc)
target_include_directories(tokenizer PRIVATE ${PROJECT_SOURCE_DIR})

add_library(type_context type_context.cc)
target_include_directories(type_context PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(type_context PRIVATE error_collector logging tokenizer)

add_library(types types.cc)
target_include_directories(types PRIVATE ${PROJECT_SOURCE_DIR})

add_executable(compiler main.cc)
target_include_directories(compiler PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(compiler PRIVATE bytecode_generator parser program_builder type_context)

include(GoogleTest)

add_executable(golden_tests golden_tests.cc)
target_link_libraries(golden_tests PRIVATE gmock_main bytecode_generator parser program_builder type_context vm)
target_include_directories(golden_tests PRIVATE ${PROJECT_SOURCE_DIR})
gtest_discover_tests(golden_tests)
add_executable(semantic_analyzer_test semantic_analyzer_test.cc)
target_link_libraries(semantic_analyzer_test PRIVATE gmock_main semantic_analyzer types)
target_include_directories(semantic_analyzer_test PRIVATE ${PROJECT_SOURCE_DIR})
gtest_discover_tests(semantic_analyzer_test)

add_executable(type_context_test type_context_test.cc)
target_link_libraries(type_context_test PRIVATE gmock_main type_context types)
target_include_directories(type_context_test PRIVATE ${PROJECT_SOURCE_DIR})
gtest_discover_tests(type_context_test)
