add_subdirectory(codegen)
add_subdirectory(parser)

add_library(error_collector error_collector.cc)
target_link_libraries(error_collector PRIVATE types)
target_include_directories(error_collector PRIVATE ${PROJECT_SOURCE_DIR})

add_library(logging logging.cc)
target_include_directories(logging PRIVATE ${PROJECT_SOURCE_DIR})

add_library(scope_manager scope_manager.cc)
target_include_directories(scope_manager PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(scope_manager PRIVATE error_collector logging types)
target_link_libraries(scope_manager PRIVATE nlohmann_json::nlohmann_json)

add_library(semantic_analyzer semantic_analyzer.cc)
target_include_directories(semantic_analyzer PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(semantic_analyzer PRIVATE error_collector logging printer scope_manager symbol_binder type_context type_registry type_resolver)

add_library(symbol_binder symbol_binder.cc)
target_include_directories(symbol_binder PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(symbol_binder PRIVATE error_collector logging printer scope_manager type_context type_registry type_resolver)

add_library(type_context type_context.cc)
target_include_directories(type_context PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(type_context PRIVATE error_collector logging scope_manager tokenizer type_registry)

add_library(type_registry type_registry.cc)
target_include_directories(type_registry PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(type_registry PRIVATE error_collector logging scope_manager tokenizer)
target_link_libraries(type_registry PRIVATE nlohmann_json::nlohmann_json)

add_library(type_resolver type_resolver.cc)
target_include_directories(type_resolver PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(type_resolver PRIVATE error_collector logging types type_context)

add_library(types types.cc)
target_include_directories(types PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(types PRIVATE logging)

add_executable(compiler main.cc)
target_include_directories(compiler PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(compiler PRIVATE bytecode_generator parser program_builder scope_manager type_context type_registry)

include(GoogleTest)

add_executable(golden_tests golden_tests.cc)
target_link_libraries(golden_tests PRIVATE gmock_main bytecode_generator parser program_builder type_context vm)
target_include_directories(golden_tests PRIVATE ${PROJECT_SOURCE_DIR})
gtest_discover_tests(golden_tests)

add_executable(scope_manager_test scope_manager_test.cc)
target_link_libraries(scope_manager_test PRIVATE gmock_main scope_manager types)
target_include_directories(scope_manager_test PRIVATE ${PROJECT_SOURCE_DIR})
gtest_discover_tests(scope_manager_test)

add_executable(semantic_analyzer_test semantic_analyzer_test.cc)
target_link_libraries(semantic_analyzer_test PRIVATE gmock_main semantic_analyzer types)
target_include_directories(semantic_analyzer_test PRIVATE ${PROJECT_SOURCE_DIR})
gtest_discover_tests(semantic_analyzer_test)

add_executable(type_context_test type_context_test.cc)
target_link_libraries(type_context_test PRIVATE gmock_main symbol_binder type_context types)
target_include_directories(type_context_test PRIVATE ${PROJECT_SOURCE_DIR})
gtest_discover_tests(type_context_test)

add_executable(type_registry_test type_registry_test.cc)
target_link_libraries(type_registry_test PRIVATE gmock_main type_registry types)
target_include_directories(type_registry_test PRIVATE ${PROJECT_SOURCE_DIR})
gtest_discover_tests(type_registry_test)

add_executable(type_resolver_test type_resolver_test.cc)
target_link_libraries(type_resolver_test PRIVATE gmock_main symbol_binder type_resolver types)
target_include_directories(type_resolver_test PRIVATE ${PROJECT_SOURCE_DIR})
gtest_discover_tests(type_resolver_test)
