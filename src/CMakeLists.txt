add_library(vm vm.c)
add_library(promise promise.c)
target_include_directories(promise PRIVATE ${CMAKE_SOURCE_DIR})

target_include_directories(vm PRIVATE ${CMAKE_SOURCE_DIR})
