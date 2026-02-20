add_library(assembler assembler.cc)
target_include_directories(assembler PRIVATE ${CMAKE_SOURCE_DIR})

add_library(logging logging.cc)
target_include_directories(logging PRIVATE ${CMAKE_SOURCE_DIR})

add_library(parser parser.cc types.cc)
target_include_directories(parser PRIVATE ${CMAKE_SOURCE_DIR})

add_library(program_builder program_builder.cc)
target_include_directories(program_builder PRIVATE ${CMAKE_SOURCE_DIR})

add_library(tokenizer tokenizer.cc)
target_include_directories(tokenizer PRIVATE ${CMAKE_SOURCE_DIR})

add_library(type_checker type_checker.cc)
target_include_directories(type_checker PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(type_checker PRIVATE logging)

add_executable(compiler main.cc)
target_include_directories(compiler PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(compiler PRIVATE assembler parser program_builder tokenizer)

