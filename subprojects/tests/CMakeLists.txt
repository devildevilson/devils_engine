cmake_minimum_required(VERSION 3.30)

project(
  devils_engine_tests
  VERSION 0.1.1
  LANGUAGES CXX C
)

set(TEST_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/tests")

function(devils_add_doctest target)
  add_executable(${target} doctest_main.cpp ${ARGN})
  target_link_libraries(${target} PRIVATE devils_engine::options doctest::doctest)
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIRECTORY}/bin")

  if (WIN32)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:${target}>
        $<TARGET_FILE_DIR:${target}>
      COMMAND_EXPAND_LISTS
    )
  endif()

  doctest_discover_tests(${target}
    TEST_PREFIX "${target}::"
    PROPERTIES LABELS devils_engine
  )
endfunction()

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")

devils_add_doctest(utils_general_test utils_general_test.cpp)
target_link_libraries(utils_general_test PRIVATE devils_engine::utils devils_engine::mood devils_engine::act)

devils_add_doctest(utils_contract_test utils_contract_test.cpp)
target_link_libraries(utils_contract_test PRIVATE devils_engine::utils)

devils_add_doctest(thread_general_test thread_general_test.cpp)
target_link_libraries(thread_general_test PRIVATE devils_engine::thread)

devils_add_doctest(aesthetics_world_test aesthetics_world_test.cpp)
target_link_libraries(aesthetics_world_test PRIVATE devils_engine::aesthetics)

devils_add_doctest(aesthetics_serialization_test aesthetics_serialization_test.cpp)
target_link_libraries(aesthetics_serialization_test PRIVATE devils_engine::aesthetics)

devils_add_doctest(sound_system_test sound_system_test.cpp)
target_link_libraries(sound_system_test PRIVATE devils_engine::sound devils_engine::utils)

devils_add_doctest(acumen_test acumen_test.cpp)
target_link_libraries(acumen_test PRIVATE devils_engine::acumen devils_engine::act)

devils_add_doctest(demiurg_resource_loader_test demiurg_resource_loader_test.cpp)
target_link_libraries(demiurg_resource_loader_test PRIVATE devils_engine::demiurg devils_engine::utils)

devils_add_doctest(flow_test flow_test.cpp)
target_link_libraries(flow_test PRIVATE devils_engine::flow devils_engine::demiurg devils_engine::utils)

devils_add_doctest(catalogue_introspection_test catalogue_introspection_test.cpp)
target_link_libraries(catalogue_introspection_test PRIVATE devils_engine::catalogue devils_engine::utils)

devils_add_doctest(painter_shader_prepare_test painter_shader_prepare_test.cpp)
target_link_libraries(painter_shader_prepare_test PRIVATE devils_engine::painter devils_engine::demiurg devils_engine::utils)

add_executable(aesthetics_world_benchmark aesthetics_world_benchmark.cpp)
target_link_libraries(aesthetics_world_benchmark PRIVATE devils_engine::options devils_engine::aesthetics)
set_target_properties(aesthetics_world_benchmark PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIRECTORY}/bin")

add_executable(aesthetics_serialization_benchmark aesthetics_serialization_benchmark.cpp)
target_link_libraries(aesthetics_serialization_benchmark PRIVATE devils_engine::options devils_engine::aesthetics)
set_target_properties(aesthetics_serialization_benchmark PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIRECTORY}/bin")

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
