cmake_minimum_required(VERSION 3.18)
project(
  lang
  VERSION 0.0.0
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Fetch the latest gtest (main) and enable testing for the project
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG main)
FetchContent_MakeAvailable(googletest)
enable_testing()

add_subdirectory(compiler)
add_subdirectory(src)
add_subdirectory(test)
