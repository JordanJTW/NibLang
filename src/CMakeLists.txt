add_library(map map.c)
target_include_directories(map PRIVATE ${CMAKE_SOURCE_DIR})

add_library(promise promise.c)
target_include_directories(promise PRIVATE ${CMAKE_SOURCE_DIR})

add_library(strings strings.c)
target_include_directories(strings PRIVATE ${CMAKE_SOURCE_DIR})

add_library(vm vm.c)
target_link_libraries(vm PRIVATE promise strings)
target_include_directories(vm PRIVATE ${CMAKE_SOURCE_DIR})
