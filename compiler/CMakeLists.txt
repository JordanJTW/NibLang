add_library(assembler assembler.cc)
target_include_directories(assembler PRIVATE ${CMAKE_SOURCE_DIR})

add_library(tokenizer tokenizer.cc)
target_include_directories(tokenizer PRIVATE ${CMAKE_SOURCE_DIR})

add_executable(compiler main.cc)
target_include_directories(compiler PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(compiler PRIVATE assembler tokenizer)

