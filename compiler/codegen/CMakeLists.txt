add_library(assembler assembler.cc)
target_link_libraries(assembler PRIVATE logging)
target_include_directories(assembler PRIVATE ${PROJECT_SOURCE_DIR})

add_library(bytecode_generator bytecode_generator.cc)
target_link_libraries(bytecode_generator PRIVATE assembler parser printer program_builder semantic_analyzer tokenizer type_context)
target_include_directories(bytecode_generator PRIVATE ${PROJECT_SOURCE_DIR})

add_library(program_builder program_builder.cc)
target_include_directories(program_builder PRIVATE ${PROJECT_SOURCE_DIR})
