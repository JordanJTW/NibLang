add_library(parser parser.cc)
target_include_directories(parser PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(parser PRIVATE error_collector logging tokenizer types)

add_library(printer printer.cc)
target_include_directories(printer PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(printer PRIVATE type_context types)

add_library(tokenizer tokenizer.cc)
target_include_directories(tokenizer PRIVATE ${PROJECT_SOURCE_DIR})

include(GoogleTest)

add_executable(parser_test parser_test.cc)
target_link_libraries(parser_test PRIVATE gmock_main error_collector logging parser)
target_include_directories(parser_test PRIVATE ${PROJECT_SOURCE_DIR})
gtest_discover_tests(parser_test)