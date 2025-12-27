add_executable(vm_test vm.cc)
target_link_libraries(vm_test PRIVATE gmock_main vm)
target_include_directories(vm_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

include(GoogleTest)
gtest_discover_tests(vm_test)
