cmake_minimum_required(VERSION 3.18)
project(
  lang
  VERSION 0.0.0
  LANGUAGES C CXX)

# Fetch the latest gtest (main) and enable testing for the project
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG main)
FetchContent_MakeAvailable(googletest)
enable_testing()

add_subdirectory(src)
add_subdirectory(test)
