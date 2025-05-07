cmake_minimum_required(VERSION 3.30)

project(
  devils_engine_tests
  VERSION 0.1.1
  LANGUAGES CXX C
)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)

add_executable(utils_general_test utils_general_test.cpp)
target_link_libraries(utils_general_test PRIVATE devils_engine::utils Catch2::Catch2WithMain)
target_link_directories(utils_general_test PRIVATE ${DENSITY_LIBRARY_DIR})

include(CTest)
include(Catch)
catch_discover_tests(utils_general_test)