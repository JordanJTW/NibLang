add_library(map map.c)
target_include_directories(map PRIVATE ${CMAKE_SOURCE_DIR})

add_library(promise promise.c)
target_include_directories(promise PRIVATE ${CMAKE_SOURCE_DIR})

add_library(vm vm.c)
target_include_directories(vm PRIVATE ${CMAKE_SOURCE_DIR})
