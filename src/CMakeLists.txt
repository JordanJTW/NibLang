add_library(array array.c)
target_include_directories(array PRIVATE ${PROJECT_SOURCE_DIR})

add_library(map map.c)
target_include_directories(map PRIVATE ${PROJECT_SOURCE_DIR})

add_library(promise promise.c)
target_include_directories(promise PRIVATE ${PROJECT_SOURCE_DIR})

add_library(strings strings.c)
target_include_directories(strings PRIVATE ${PROJECT_SOURCE_DIR})

add_library(vm vm.c)
target_link_libraries(vm PRIVATE array map promise strings)
target_include_directories(vm PUBLIC ${PROJECT_SOURCE_DIR})

add_executable(run main.c)
target_link_libraries(run PRIVATE vm)
target_include_directories(run PRIVATE ${PROJECT_SOURCE_DIR})
