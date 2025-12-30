add_library(assembler assembler.cc)
target_include_directories(assembler PRIVATE ${CMAKE_SOURCE_DIR})

add_executable(vm_test vm.cc)
target_link_libraries(vm_test PRIVATE assembler gmock_main vm)
target_include_directories(vm_test PRIVATE ${CMAKE_SOURCE_DIR})

include(GoogleTest)
gtest_discover_tests(vm_test)
