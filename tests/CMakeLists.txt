cmake_minimum_required(VERSION 3.30)

project(
  devils_engine_tests
  VERSION 0.1.1
  LANGUAGES CXX C
)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)

set(TEST_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/devils_engine/tests")

add_executable(utils_general_test utils_general_test.cpp)
target_link_libraries(utils_general_test PRIVATE devils_engine::options devils_engine::utils Catch2::Catch2WithMain)
target_link_directories(utils_general_test PRIVATE ${DENSITY_LIBRARY_DIR})
set_target_properties(utils_general_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIRECTORY}/bin")

add_custom_command(TARGET utils_general_test POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory 
    ${CMAKE_CURRENT_LIST_DIR}/test_render_config 
    ${TEST_OUTPUT_DIRECTORY}/render_config
)

add_custom_command(TARGET utils_general_test POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory 
    ${CMAKE_CURRENT_LIST_DIR}/shaders 
    ${TEST_OUTPUT_DIRECTORY}/shaders
)

if (WIN32)
  add_custom_command(TARGET utils_general_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:utils_general_test>
      $<TARGET_FILE_DIR:utils_general_test>
    COMMAND_EXPAND_LISTS
  )
endif()

include(CTest)
include(Catch)
catch_discover_tests(utils_general_test)