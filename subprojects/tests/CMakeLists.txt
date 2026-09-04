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

devils_add_doctest(timeline_test timeline_test.cpp)
target_link_libraries(timeline_test PRIVATE devils_engine::utils)

devils_add_doctest(simulation_time_test simulation_time_test.cpp)
target_link_libraries(simulation_time_test PRIVATE devils_engine::utils)

devils_add_doctest(gameplay_timeline_test gameplay_timeline_test.cpp)
target_link_libraries(gameplay_timeline_test PRIVATE devils_engine::simul)

devils_add_doctest(tile_frontier_config_test tile_frontier_config_test.cpp ../tile_frontier/src/core/config.cpp)
target_link_libraries(tile_frontier_config_test PRIVATE devils_engine::utils devils_engine::input tavl)
target_include_directories(tile_frontier_config_test PRIVATE
  "${CMAKE_SOURCE_DIR}/subprojects/tile_frontier/include"
  "${CMAKE_SOURCE_DIR}/subprojects/tile_frontier/include/core"
)

devils_add_doctest(thread_general_test thread_general_test.cpp)
target_link_libraries(thread_general_test PRIVATE devils_engine::thread)

# PRE-01 from NETWORKING.md deliberately probes the third-party transport before
# a production adapter exists.  It links GNS directly and moves no GNS type into
# an engine library.
devils_add_doctest(gamenetworking_sockets_capability_test gamenetworking_sockets_capability_test.cpp)
target_link_libraries(gamenetworking_sockets_capability_test PRIVATE GameNetworkingSockets::static)

devils_add_doctest(network_tick_journal_test network_tick_journal_test.cpp)
target_link_libraries(network_tick_journal_test PRIVATE devils_engine::network)

devils_add_doctest(network_sequence_history_test network_sequence_history_test.cpp)
target_link_libraries(network_sequence_history_test PRIVATE devils_engine::network)

devils_add_doctest(network_state_schema_test network_state_schema_test.cpp)
target_link_libraries(network_state_schema_test PRIVATE devils_engine::network)

devils_add_doctest(network_checkpoint_replay_test network_checkpoint_replay_test.cpp)
target_link_libraries(network_checkpoint_replay_test PRIVATE devils_engine::network)

devils_add_doctest(network_state_digest_test network_state_digest_test.cpp)
target_link_libraries(network_state_digest_test PRIVATE devils_engine::network)

find_program(DEVILS_NETWORK_GXX NAMES g++)
find_program(DEVILS_NETWORK_CLANGXX NAMES clang++)
if (DEVILS_NETWORK_GXX AND DEVILS_NETWORK_CLANGXX)
  set(DEVILS_NETWORK_NATIVE_FLOAT_ARCH_FLAG "")
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i[3-6]86)$")
    set(DEVILS_NETWORK_NATIVE_FLOAT_ARCH_FLAG "-mavx")
  endif()
  add_test(NAME network_native_float_cross_compiler_probe
    COMMAND ${CMAKE_COMMAND}
      -DPROBE_SOURCE=${CMAKE_CURRENT_LIST_DIR}/network_native_float_corpus.cpp
      -DPROBE_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/network_native_float_probe
      -DPROBE_ARCH_FLAG=${DEVILS_NETWORK_NATIVE_FLOAT_ARCH_FLAG}
      -DGXX=${DEVILS_NETWORK_GXX}
      -DCLANGXX=${DEVILS_NETWORK_CLANGXX}
      -P ${CMAKE_CURRENT_LIST_DIR}/network_cross_compiler_probe.cmake
  )
  set_tests_properties(network_native_float_cross_compiler_probe PROPERTIES LABELS devils_engine)
else()
  message(STATUS "network native-float cross-compiler probe disabled: g++ or clang++ not found")
endif()

devils_add_doctest(resolve_pipeline_test resolve_pipeline_test.cpp)
target_link_libraries(resolve_pipeline_test PRIVATE devils_engine::resolve devils_engine::thread)

devils_add_doctest(input_bindings_test input_bindings_test.cpp)
target_link_libraries(input_bindings_test PRIVATE devils_engine::input devils_engine::utils tavl)

devils_add_doctest(simul_lifecycle_test simul_lifecycle_test.cpp)
target_link_libraries(simul_lifecycle_test PRIVATE devils_engine::simul)

devils_add_doctest(turn_pipeline_test turn_pipeline_test.cpp)
target_link_libraries(turn_pipeline_test PRIVATE devils_engine::simul)

devils_add_doctest(simul_settings_files_test simul_settings_files_test.cpp)
target_link_libraries(simul_settings_files_test PRIVATE devils_engine::simul tavl)

devils_add_doctest(aesthetics_world_test aesthetics_world_test.cpp)
target_link_libraries(aesthetics_world_test PRIVATE devils_engine::aesthetics)

devils_add_doctest(aesthetics_message_buffer_test aesthetics_message_buffer_test.cpp)
target_link_libraries(aesthetics_message_buffer_test PRIVATE devils_engine::aesthetics devils_engine::thread)

devils_add_doctest(aesthetics_pipeline_test aesthetics_pipeline_test.cpp)
target_link_libraries(aesthetics_pipeline_test PRIVATE devils_engine::aesthetics devils_engine::thread)

devils_add_doctest(aesthetics_elect_buffer_test aesthetics_elect_buffer_test.cpp)
target_link_libraries(aesthetics_elect_buffer_test PRIVATE devils_engine::aesthetics devils_engine::thread)

devils_add_doctest(aesthetics_interaction_arena_test aesthetics_interaction_arena_test.cpp)
target_link_libraries(aesthetics_interaction_arena_test PRIVATE devils_engine::aesthetics devils_engine::thread)

devils_add_doctest(aesthetics_collect_buffer_test aesthetics_collect_buffer_test.cpp)
target_link_libraries(aesthetics_collect_buffer_test PRIVATE devils_engine::aesthetics devils_engine::thread)

devils_add_doctest(aesthetics_flag_set_test aesthetics_flag_set_test.cpp)
target_link_libraries(aesthetics_flag_set_test PRIVATE devils_engine::aesthetics)

devils_add_doctest(catalogue_call_log_test catalogue_call_log_test.cpp)
target_link_libraries(catalogue_call_log_test PRIVATE devils_engine::catalogue devils_engine::thread)

devils_add_doctest(catalogue_deferred_test catalogue_deferred_test.cpp)
target_link_libraries(catalogue_deferred_test PRIVATE devils_engine::catalogue devils_engine::thread devils_script)

devils_add_doctest(catalogue_phase_metadata_test catalogue_phase_metadata_test.cpp)
target_link_libraries(catalogue_phase_metadata_test PRIVATE devils_engine::catalogue)

devils_add_doctest(aesthetics_serialization_test aesthetics_serialization_test.cpp)
target_link_libraries(aesthetics_serialization_test PRIVATE devils_engine::aesthetics)

devils_add_doctest(sound_system_test sound_system_test.cpp)
target_link_libraries(sound_system_test PRIVATE devils_engine::sound devils_engine::utils devils_engine::demiurg)

devils_add_doctest(acumen_test acumen_test.cpp)
target_link_libraries(acumen_test PRIVATE devils_engine::acumen devils_engine::act)

devils_add_doctest(acumen_script_test acumen_script_test.cpp)
target_link_libraries(acumen_script_test PRIVATE devils_engine::acumen devils_engine::act devils_script tavl)

devils_add_doctest(goap_config_test goap_config_test.cpp)
target_link_libraries(goap_config_test PRIVATE devils_engine::acumen devils_engine::act devils_engine::demiurg devils_script tavl)

devils_add_doctest(fsm_config_test fsm_config_test.cpp)
target_link_libraries(fsm_config_test PRIVATE devils_engine::mood devils_engine::act devils_engine::demiurg tavl)

devils_add_doctest(mood_diagnostics_test mood_diagnostics_test.cpp)
target_link_libraries(mood_diagnostics_test PRIVATE devils_engine::mood devils_engine::act)

devils_add_doctest(act_call_context_test act_call_context_test.cpp)
target_link_libraries(act_call_context_test PRIVATE devils_engine::act devils_script)

devils_add_doctest(act_packer_test act_packer_test.cpp)
target_link_libraries(act_packer_test PRIVATE devils_engine::act devils_script)

devils_add_doctest(act_building_blocks_test act_building_blocks_test.cpp)
target_link_libraries(act_building_blocks_test PRIVATE devils_engine::act devils_engine::catalogue devils_script)

devils_add_doctest(stats_script_test stats_script_test.cpp)
target_link_libraries(stats_script_test PRIVATE devils_engine::act)

devils_add_doctest(prefab_test prefab_test.cpp)
target_link_libraries(prefab_test PRIVATE devils_engine::prefab)

devils_add_doctest(spawn_script_test spawn_script_test.cpp)
target_link_libraries(spawn_script_test PRIVATE devils_script tavl glm::glm devils_engine::aesthetics devils_engine::utils)
target_include_directories(spawn_script_test PRIVATE "${CMAKE_SOURCE_DIR}/subprojects/tile_frontier/include")

devils_add_doctest(demiurg_resource_loader_test demiurg_resource_loader_test.cpp)
target_link_libraries(demiurg_resource_loader_test PRIVATE devils_engine::demiurg devils_engine::utils)

devils_add_doctest(flow_test flow_test.cpp)
target_link_libraries(flow_test PRIVATE devils_engine::flow devils_engine::demiurg devils_engine::utils)

devils_add_doctest(catalogue_introspection_test catalogue_introspection_test.cpp)
target_link_libraries(catalogue_introspection_test PRIVATE devils_engine::catalogue devils_engine::utils)

# Вычислительный контекст: Vulkan без окна и без render-графа. Тест ОХРАНЯЕМЫЙ — отсутствие
# устройства законно, и тогда он просто ничего не проверяет.
devils_add_doctest(painter_compute_context_test painter_compute_context_test.cpp)
target_link_libraries(painter_compute_context_test PRIVATE devils_engine::painter devils_engine::utils)

devils_add_doctest(painter_shader_prepare_test painter_shader_prepare_test.cpp)
target_link_libraries(painter_shader_prepare_test PRIVATE devils_engine::painter devils_engine::demiurg devils_engine::utils)
target_compile_definitions(painter_shader_prepare_test PRIVATE
  PAINTER_TEST_CONFIG_ROOT="${CMAKE_CURRENT_LIST_DIR}/test_render_config/"
)

devils_add_doctest(painter_atlas_layout_test painter_atlas_layout_test.cpp)
target_link_libraries(painter_atlas_layout_test PRIVATE devils_engine::painter)

devils_add_doctest(painter_temporal_history_test painter_temporal_history_test.cpp)
target_link_libraries(painter_temporal_history_test PRIVATE devils_engine::painter)

devils_add_doctest(painter_queue_plan_test painter_queue_plan_test.cpp)
target_link_libraries(painter_queue_plan_test PRIVATE devils_engine::painter)

devils_add_doctest(painter_asset_lifetime_test painter_asset_lifetime_test.cpp)
target_link_libraries(painter_asset_lifetime_test PRIVATE devils_engine::painter devils_engine::demiurg devils_engine::utils)

add_executable(aesthetics_world_benchmark aesthetics_world_benchmark.cpp)
target_link_libraries(aesthetics_world_benchmark PRIVATE devils_engine::options devils_engine::aesthetics)
set_target_properties(aesthetics_world_benchmark PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIRECTORY}/bin")

add_executable(aesthetics_serialization_benchmark aesthetics_serialization_benchmark.cpp)
target_link_libraries(aesthetics_serialization_benchmark PRIVATE devils_engine::options devils_engine::aesthetics)
set_target_properties(aesthetics_serialization_benchmark PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIRECTORY}/bin")

add_executable(script_vs_native_benchmark script_vs_native_benchmark.cpp)
target_link_libraries(script_vs_native_benchmark PRIVATE devils_engine::options devils_script tavl)
set_target_properties(script_vs_native_benchmark PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIRECTORY}/bin")

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

devils_add_doctest(originator_buffer_test originator_buffer_test.cpp)
target_link_libraries(originator_buffer_test PRIVATE devils_engine::originator)

devils_add_doctest(originator_tools_test originator_tools_test.cpp)
target_link_libraries(originator_tools_test PRIVATE devils_engine::originator devils_engine::utils)

devils_add_doctest(originator_pipeline_test originator_pipeline_test.cpp)
target_link_libraries(originator_pipeline_test PRIVATE devils_engine::originator devils_engine::utils)

devils_add_doctest(originator_queue_test originator_queue_test.cpp)
target_link_libraries(originator_queue_test PRIVATE devils_engine::originator devils_engine::utils)

# Lua-поверхность очереди. Отдельная цель потому, что она линкует скриптовый слой: ядру генератора
# lua не нужна, а этому тесту нужна — он проверяет и то, что регистрация неймспейса не портит память.
devils_add_doctest(originator_queue_lua_test originator_queue_lua_test.cpp)
target_link_libraries(originator_queue_lua_test PRIVATE devils_engine::originator devils_engine::originator_lua devils_engine::utils)

devils_add_doctest(originator_config_test originator_config_test.cpp)
target_link_libraries(originator_config_test PRIVATE devils_engine::originator devils_engine::originator_config devils_engine::demiurg devils_engine::utils)

devils_add_doctest(originator_script_test originator_script_test.cpp)
target_link_libraries(originator_script_test PRIVATE devils_engine::originator devils_engine::originator_script devils_engine::utils)

# Транслятор ds -> GLSL и сверка двух путей. Линкует и скриптовый слой, и painter: одна программа,
# два исполнения. Часть с устройством ОХРАНЯЕМАЯ.
devils_add_doctest(originator_translate_test originator_translate_test.cpp)
target_link_libraries(originator_translate_test PRIVATE devils_engine::originator devils_engine::originator_script devils_engine::painter devils_engine::utils)

devils_add_doctest(originator_primitives_test originator_primitives_test.cpp)
target_link_libraries(originator_primitives_test PRIVATE devils_engine::originator devils_engine::originator_primitives devils_engine::utils)

devils_add_doctest(originator_scatter_test originator_scatter_test.cpp)
target_link_libraries(originator_scatter_test PRIVATE devils_engine::originator devils_engine::utils)

devils_add_doctest(originator_volume_test originator_volume_test.cpp)
target_link_libraries(originator_volume_test PRIVATE devils_engine::originator devils_engine::utils)
