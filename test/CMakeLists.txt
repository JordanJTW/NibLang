add_library(assembler assembler.cc)
target_include_directories(assembler PRIVATE ${CMAKE_SOURCE_DIR})

add_executable(json_parser json_parser.cc)
target_link_libraries(json_parser PRIVATE assembler vm)
target_include_directories(json_parser PRIVATE ${CMAKE_SOURCE_DIR})

include(GoogleTest)

add_executable(promise_test promise_test.cc gtest_helpers.h)
target_link_libraries(promise_test PRIVATE gmock_main vm)
target_include_directories(promise_test PRIVATE ${CMAKE_SOURCE_DIR})
gtest_discover_tests(promise_test)

add_executable(map_test map_test.cc)
target_link_libraries(map_test PRIVATE gmock_main map vm)
target_include_directories(map_test PRIVATE ${CMAKE_SOURCE_DIR})
gtest_discover_tests(map_test)

add_executable(vm_test vm_test.cc gtest_helpers.h)
target_link_libraries(vm_test PRIVATE assembler gmock_main vm)
target_include_directories(vm_test PRIVATE ${CMAKE_SOURCE_DIR})
gtest_discover_tests(vm_test)
