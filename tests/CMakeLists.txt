cmake_minimum_required(VERSION 3.30)

project(
  devils_engine_tests
  VERSION 0.1.1
  LANGUAGES CXX C
)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)

add_executable(utils_general_test utils_general_test.cpp)
target_link_libraries(utils_general_test PRIVATE devils_engine::options devils_engine::utils Catch2::Catch2WithMain)
target_link_directories(utils_general_test PRIVATE ${DENSITY_LIBRARY_DIR})

add_custom_target(copy_render_config
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_LIST_DIR}/test_render_config ${DEVILS_ENGINE_OUTPUT_DIRECTORY}/tests/render_config
)

add_custom_target(copy_shaders
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_LIST_DIR}/shaders ${DEVILS_ENGINE_OUTPUT_DIRECTORY}/tests/shaders
)

add_dependencies(utils_general_test copy_render_config)
add_dependencies(utils_general_test copy_shaders)

include(CTest)
include(Catch)
catch_discover_tests(utils_general_test)