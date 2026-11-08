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

  # Собираем имена здесь, а не переписываем список DEPENDS в корне: раньше он
  # отставал молча, и новый тест просто не собирался целью devils_engine_test.
  set_property(GLOBAL APPEND PROPERTY DEVILS_ENGINE_TEST_TARGETS ${target})
endfunction()

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")

devils_add_doctest(utils_general_test utils_general_test.cpp)
target_link_libraries(utils_general_test PRIVATE devils_engine::utils devils_engine::mood devils_engine::act)

devils_add_doctest(utils_contract_test utils_contract_test.cpp)
target_link_libraries(utils_contract_test PRIVATE devils_engine::utils)

devils_add_doctest(utils_serialization_test utils_serialization_test.cpp)
target_link_libraries(utils_serialization_test PRIVATE devils_engine::utils)

devils_add_doctest(timeline_test timeline_test.cpp)
target_link_libraries(timeline_test PRIVATE devils_engine::utils)

devils_add_doctest(simulation_time_test simulation_time_test.cpp)
target_link_libraries(simulation_time_test PRIVATE devils_engine::utils)

devils_add_doctest(gameplay_timeline_test gameplay_timeline_test.cpp)
target_link_libraries(gameplay_timeline_test PRIVATE devils_engine::simul)

devils_add_doctest(tile_frontier_config_test tile_frontier_config_test.cpp ../tile_frontier/src/core/config.cpp)
target_link_libraries(tile_frontier_config_test PRIVATE devils_engine::utils devils_engine::input devils_engine::simul tavl)
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

devils_add_doctest(network_replication_test network_replication_test.cpp)
target_link_libraries(network_replication_test PRIVATE devils_engine::network)

devils_add_doctest(network_hot_path_test network_hot_path_test.cpp)
target_link_libraries(network_hot_path_test PRIVATE devils_engine::network)

devils_add_doctest(network_session_test network_session_test.cpp)
target_link_libraries(network_session_test PRIVATE devils_engine::network)

devils_add_doctest(network_session_wire_test network_session_wire_test.cpp)
target_link_libraries(network_session_wire_test PRIVATE devils_engine::network)

devils_add_doctest(network_intent_wire_test network_intent_wire_test.cpp)
target_link_libraries(network_intent_wire_test PRIVATE devils_engine::network)
devils_add_doctest(network_canonical_tick_wire_test network_canonical_tick_wire_test.cpp)
target_link_libraries(network_canonical_tick_wire_test PRIVATE devils_engine::network)

devils_add_doctest(network_transform_wire_test network_transform_wire_test.cpp)
target_link_libraries(network_transform_wire_test PRIVATE devils_engine::network)

devils_add_doctest(network_credential_test network_credential_test.cpp)
target_link_libraries(network_credential_test PRIVATE devils_engine::network)

devils_add_doctest(network_reconnect_test network_reconnect_test.cpp)
target_link_libraries(network_reconnect_test PRIVATE devils_engine::network)

if (TARGET devils_engine::network_gns)
  devils_add_doctest(network_gns_transport_test network_gns_transport_test.cpp)
  target_link_libraries(network_gns_transport_test PRIVATE devils_engine::network_gns)
  devils_add_doctest(network_backend_session_test network_backend_session_test.cpp)
  target_link_libraries(network_backend_session_test PRIVATE devils_engine::network devils_engine::network_gns)
endif()

find_program(DEVILS_NETWORK_GXX NAMES g++)
find_program(DEVILS_NETWORK_CLANGXX NAMES clang++)
if (DEVILS_NETWORK_GXX AND DEVILS_NETWORK_CLANGXX)
  set(DEVILS_NETWORK_NATIVE_FLOAT_ARCH_FLAG "")
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i[3-6]86)$")
    set(DEVILS_NETWORK_NATIVE_FLOAT_ARCH_FLAG "-mavx")
  endif()
  devils_add_test(NAME network_native_float_cross_compiler_probe
    COMMAND ${CMAKE_COMMAND}
      -DPROBE_SOURCE=${CMAKE_CURRENT_LIST_DIR}/network_native_float_corpus.cpp
      -DPROBE_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/network_native_float_probe
      -DPROBE_ARCH_FLAG=${DEVILS_NETWORK_NATIVE_FLOAT_ARCH_FLAG}
      -DGXX=${DEVILS_NETWORK_GXX}
      -DCLANGXX=${DEVILS_NETWORK_CLANGXX}
      -P ${CMAKE_CURRENT_LIST_DIR}/network_cross_compiler_probe.cmake
  )
  set_tests_properties(network_native_float_cross_compiler_probe PROPERTIES LABELS devils_engine)

  devils_add_test(NAME utils_deterministic_cross_compiler_probe
    COMMAND ${CMAKE_COMMAND}
      -DPROBE_SOURCE=${CMAKE_CURRENT_LIST_DIR}/utils_deterministic_corpus.cpp
      -DPROBE_INCLUDE_DIR=${CMAKE_SOURCE_DIR}/libs/utils/include
      -DPROBE_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/utils_deterministic_probe
      -DGXX=${DEVILS_NETWORK_GXX}
      -DCLANGXX=${DEVILS_NETWORK_CLANGXX}
      -P ${CMAKE_CURRENT_LIST_DIR}/utils_deterministic_cross_compiler_probe.cmake)
  set_tests_properties(utils_deterministic_cross_compiler_probe PROPERTIES LABELS devils_engine)
else()
  message(STATUS "network native-float cross-compiler probe disabled: g++ or clang++ not found")
endif()

devils_add_doctest(resolve_pipeline_test resolve_pipeline_test.cpp)
target_link_libraries(resolve_pipeline_test PRIVATE devils_engine::resolve devils_engine::thread)

devils_add_doctest(input_bindings_test input_bindings_test.cpp)
target_link_libraries(input_bindings_test PRIVATE devils_engine::input devils_engine::utils tavl)

# Проверка списка свойств GN06 без окна/GPU: панель принимает ограниченное число строк,
# но у одной области может быть сколько угодно частей и соединений.
devils_add_doctest(gn06_viewer_details_test gn06_viewer_details_test.cpp
  ../playgrounds/GN06_dungeon_layout/src/viewer_detail_page.cpp)

devils_add_doctest(gn06_motif_resource_test gn06_motif_resource_test.cpp
  ../playgrounds/GN06_dungeon_layout/src/site_motif_resource.cpp)
target_link_libraries(gn06_motif_resource_test PRIVATE devils_engine::originator tavl)
target_compile_definitions(gn06_motif_resource_test PRIVATE
  GN06_TEST_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../playgrounds/GN06_dungeon_layout/resources/gn06")

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

devils_add_doctest(originator_profile_test originator_profile_test.cpp)
target_link_libraries(originator_profile_test PRIVATE devils_engine::originator devils_engine::utils)

devils_add_doctest(originator_collapse_test originator_collapse_test.cpp)
target_link_libraries(originator_collapse_test PRIVATE devils_engine::originator devils_engine::utils)

devils_add_doctest(originator_structure_test originator_structure_test.cpp)
target_link_libraries(originator_structure_test PRIVATE devils_engine::originator devils_engine::utils)

devils_add_doctest(originator_motif_test originator_motif_test.cpp)
target_link_libraries(originator_motif_test PRIVATE devils_engine::originator devils_engine::utils)

devils_add_doctest(originator_motif_geometry_test originator_motif_geometry_test.cpp)
target_link_libraries(originator_motif_geometry_test PRIVATE devils_engine::originator)

# Lua-поверхность очереди. Отдельная цель потому, что она линкует скриптовый слой: ядру генератора
# lua не нужна, а этому тесту нужна — он проверяет и то, что регистрация неймспейса не портит память.
devils_add_doctest(originator_queue_lua_test originator_queue_lua_test.cpp)
target_link_libraries(originator_queue_lua_test PRIVATE devils_engine::originator devils_engine::originator_lua devils_engine::utils)

devils_add_doctest(originator_config_test originator_config_test.cpp)
target_link_libraries(originator_config_test PRIVATE devils_engine::originator devils_engine::originator_config devils_engine::demiurg devils_engine::utils)

devils_add_doctest(originator_script_test originator_script_test.cpp)
target_link_libraries(originator_script_test PRIVATE devils_engine::originator devils_engine::originator_script devils_engine::utils)
devils_add_doctest(gn02_formula_test gn02_formula_test.cpp fixtures/gn02_reference_tools.cpp)
target_link_libraries(gn02_formula_test PRIVATE devils_engine::originator_script)
target_compile_definitions(gn02_formula_test PRIVATE
  GN02_FORMULA_ROOT="${CMAKE_SOURCE_DIR}/subprojects/playgrounds/GN02_planet_generator/resources/gn02/generator/scripts")

# Транслятор ds -> GLSL и сверка двух путей. Линкует и скриптовый слой, и painter: одна программа,
# два исполнения. Часть с устройством ОХРАНЯЕМАЯ.
# Очередь, СОСТАВЛЕННАЯ для устройства из самой себя: выведенная передача, выведенные барьеры, один
# результат на двух путях. Часть с устройством ОХРАНЯЕМАЯ, а проверка плана работает всюду.
devils_add_doctest(originator_device_queue_test originator_device_queue_test.cpp)
target_link_libraries(originator_device_queue_test PRIVATE devils_engine::originator devils_engine::originator_device devils_engine::utils)

# Очередь перед устройством: gather, роды полей и раскладка, кэш шейдеров. Всё измеряемое, поэтому
# живёт тестом, а не рассуждением. Часть с устройством ОХРАНЯЕМАЯ.
devils_add_doctest(originator_device_test originator_device_test.cpp)
target_link_libraries(originator_device_test PRIVATE devils_engine::originator devils_engine::painter devils_engine::utils)

devils_add_doctest(originator_translate_test originator_translate_test.cpp)
target_link_libraries(originator_translate_test PRIVATE devils_engine::originator devils_engine::originator_script devils_engine::painter devils_engine::utils)

devils_add_doctest(originator_column_test originator_column_test.cpp)
target_link_libraries(originator_column_test PRIVATE devils_engine::originator devils_engine::originator_script devils_engine::utils)

devils_add_doctest(originator_primitives_test originator_primitives_test.cpp)
target_link_libraries(originator_primitives_test PRIVATE devils_engine::originator devils_engine::originator_primitives devils_engine::utils)

devils_add_doctest(originator_scatter_test originator_scatter_test.cpp)
target_link_libraries(originator_scatter_test PRIVATE devils_engine::originator devils_engine::utils)

devils_add_doctest(originator_chain_test originator_chain_test.cpp)
target_link_libraries(originator_chain_test PRIVATE devils_engine::originator devils_engine::originator_lua devils_engine::utils)

devils_add_doctest(originator_surface_test originator_surface_test.cpp)
target_link_libraries(originator_surface_test PRIVATE devils_engine::originator devils_engine::utils)

devils_add_doctest(originator_field_test originator_field_test.cpp)
target_link_libraries(originator_field_test PRIVATE devils_engine::originator devils_engine::originator_lua devils_engine::utils)

devils_add_doctest(originator_neighbourhood_test originator_neighbourhood_test.cpp)
target_link_libraries(originator_neighbourhood_test PRIVATE devils_engine::originator devils_engine::utils)

devils_add_doctest(originator_skeleton_test originator_skeleton_test.cpp)
target_link_libraries(originator_skeleton_test PRIVATE devils_engine::originator devils_engine::utils)

devils_add_doctest(originator_volume_test originator_volume_test.cpp)
target_link_libraries(originator_volume_test PRIVATE devils_engine::originator devils_engine::utils)

# Native critical errors and raw data conversion share the headless Lua boundary.
devils_add_doctest(bindings_data_test bindings_data_test.cpp)
target_link_libraries(bindings_data_test PRIVATE devils_engine::bindings_base)

devils_add_doctest(fixed_pool_move_test fixed_pool_move_test.cpp)
target_link_libraries(fixed_pool_move_test PRIVATE devils_engine::utils)

devils_add_doctest(demiurg_profile_test demiurg_profile_test.cpp)
target_link_libraries(demiurg_profile_test PRIVATE devils_engine::demiurg minizip)

find_package(Python3 QUIET COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
  devils_add_test(NAME module_profile_migration
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/test_convert_module_profile.py)
  set_tests_properties(module_profile_migration PROPERTIES LABELS devils_engine)
endif()

# Narrow utility headers are intentional standalone entry points after core.h split.
devils_add_doctest(utils_core_headers_test utils_core_headers_test.cpp)
target_link_libraries(utils_core_headers_test PRIVATE devils_engine::utils)

foreach(header IN ITEMS core error log print trace numeric platform unicode crc32c)
  add_library(utils_${header}_header_compile OBJECT utils_header_compile.cpp)
  target_link_libraries(utils_${header}_header_compile PRIVATE devils_engine::utils devils_engine::options)
  target_compile_definitions(utils_${header}_header_compile PRIVATE
    DEVILS_UTILS_HEADER_UNDER_TEST="devils_engine/utils/${header}.h")
  add_dependencies(utils_core_headers_test utils_${header}_header_compile)
endforeach()

devils_add_doctest(painter_memory_statistics_test painter_memory_statistics_test.cpp)
target_link_libraries(painter_memory_statistics_test PRIVATE devils_engine::painter devils_engine::utils)

if (UNIX AND NOT APPLE)
  # Explicit profiling tool, never an automatic performance acceptance test.
  add_executable(sound_cpu_probe sound_cpu_probe.cpp)
  target_link_libraries(sound_cpu_probe PRIVATE devils_engine::sound devils_engine::utils devils_engine::options)
  target_compile_definitions(sound_cpu_probe PRIVATE
    SOUND_PROBE_MP3="${CMAKE_SOURCE_DIR}/subprojects/tile_frontier/resources/modules/core/sounds/ambient/soundreality-ambient-spring-forest-323801.mp3")
  set_target_properties(sound_cpu_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIRECTORY}/bin")
endif()

# Real bundled resources and one-time user debug migration across the standard bootstrap.
devils_add_doctest(simul_configuration_layers_test simul_configuration_layers_test.cpp
  ../tile_frontier/src/core/app_config_resource.cpp)
target_link_libraries(simul_configuration_layers_test PRIVATE devils_engine::simul)
target_include_directories(simul_configuration_layers_test PRIVATE
  "${CMAKE_SOURCE_DIR}/subprojects/tile_frontier/include"
  "${CMAKE_SOURCE_DIR}/subprojects/tile_frontier/include/core")
target_compile_definitions(simul_configuration_layers_test PRIVATE
  ENGINE_CONFIG_TEST_ROOT="${CMAKE_SOURCE_DIR}/resources/")

# Public usage requirements: no options/aggregate target is added by these consumers.
add_custom_target(devils_public_headers)
foreach(library IN ITEMS demiurg input sound visage)
  file(GLOB public_headers CONFIGURE_DEPENDS RELATIVE "${CMAKE_SOURCE_DIR}/libs/${library}/include/"
    "${CMAKE_SOURCE_DIR}/libs/${library}/include/devils_engine/${library}/*.h")
  foreach(header IN LISTS public_headers)
    get_filename_component(header_name "${header}" NAME_WE)
    set(check_target "public_header_${library}_${header_name}")
    add_library(${check_target} OBJECT EXCLUDE_FROM_ALL public_header_compile.cpp)
    target_link_libraries(${check_target} PRIVATE devils_engine::${library})
    target_compile_definitions(${check_target} PRIVATE DEVILS_PUBLIC_HEADER="${header}")
    add_dependencies(devils_public_headers ${check_target})
  endforeach()
  add_executable(public_link_${library} public_link_${library}.cpp)
  target_link_libraries(public_link_${library} PRIVATE devils_engine::${library})
  if(WIN32)
    add_custom_command(TARGET public_link_${library} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:public_link_${library}>
        $<TARGET_FILE_DIR:public_link_${library}>
      COMMAND_EXPAND_LISTS)
  endif()
  set_target_properties(public_link_${library} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIRECTORY}/bin")
  devils_add_test(NAME public_link_${library} COMMAND public_link_${library})
  set_tests_properties(public_link_${library} PROPERTIES LABELS devils_engine)
  add_dependencies(devils_public_headers public_link_${library})
endforeach()

# Проверки узких границ нового generation API; каждый header виден через своего владельца.
foreach(header IN ITEMS graph_walk neighbourhood skeleton script_program script_column script_translate)
  set(check_target "public_header_originator_${header}")
  add_library(${check_target} OBJECT EXCLUDE_FROM_ALL public_header_compile.cpp)
  if(header MATCHES "^script_")
    target_link_libraries(${check_target} PRIVATE devils_engine::originator_script)
  else()
    target_link_libraries(${check_target} PRIVATE devils_engine::originator)
  endif()
  target_compile_definitions(${check_target} PRIVATE DEVILS_PUBLIC_HEADER="devils_engine/originator/${header}.h")
  add_dependencies(devils_public_headers ${check_target})
endforeach()
foreach(header IN ITEMS chunk_streaming world_memory)
  set(check_target "public_header_playground_${header}")
  add_library(${check_target} OBJECT EXCLUDE_FROM_ALL public_header_compile.cpp)
  target_link_libraries(${check_target} PRIVATE devils_engine::playground_streaming)
  target_compile_definitions(${check_target} PRIVATE DEVILS_PUBLIC_HEADER="devils_engine/playground/${header}.h")
  add_dependencies(devils_public_headers ${check_target})
endforeach()

if(Python3_Interpreter_FOUND)
  devils_add_test(NAME test_build_dependencies_contract
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_test_build_dependencies.py ${CMAKE_BINARY_DIR})
  set_tests_properties(test_build_dependencies_contract PROPERTIES LABELS devils_engine)
endif()

if(Python3_Interpreter_FOUND)
  devils_add_test(NAME project_task_ids_contract
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/inventory_project_tasks.py --check)
  set_tests_properties(project_task_ids_contract PROPERTIES LABELS devils_engine)
endif()
