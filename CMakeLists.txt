cmake_minimum_required(VERSION 3.18)
project(
  lang
  VERSION 0.0.0
  LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_ASAN "enables Address Sanitizer in the build" OFF)

if(ENABLE_ASAN)
  message(STATUS "Enabling Address Sanitizer")

  set(ASAN_FLAGS "-fsanitize=address" "-fno-omit-frame-pointer")
  set(DEBUG_FLAGS "-O0" "-g")

  add_compile_options(${ASAN_FLAGS} ${DEBUG_FLAGS})
  add_link_options(${ASAN_FLAGS})
  set(CMAKE_XCODE_SCHEME_ADDRESS_SANITIZER ON)
endif()

# Fetch the latest gtest (main) and enable testing for the project
include(FetchContent)
fetchcontent_declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG main)
fetchcontent_makeavailable(googletest)
enable_testing()

add_subdirectory(compiler)
add_subdirectory(src)
add_subdirectory(test)
