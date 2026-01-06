add_library(assembler assembler.cc)
target_include_directories(assembler PRIVATE ${CMAKE_SOURCE_DIR})

add_executable(promise_test promise.cc)
target_link_libraries(promise_test PRIVATE gmock_main promise vm)
target_include_directories(promise_test PRIVATE ${CMAKE_SOURCE_DIR})
add_executable(vm_test vm.cc)
target_link_libraries(vm_test PRIVATE assembler gmock_main vm)
target_include_directories(vm_test PRIVATE ${CMAKE_SOURCE_DIR})

include(GoogleTest)
gtest_discover_tests(promise_test vm_test)
