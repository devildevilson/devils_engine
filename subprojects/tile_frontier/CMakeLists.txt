cmake_minimum_required(VERSION 3.30)

project(
  tile_frontier
  VERSION 0.1.1
  LANGUAGES CXX C
)

set(PROJECT_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/${PROJECT_NAME}")

set(PROJECT_EXTERNAL_DEPENDENCY_DIR ${CMAKE_CURRENT_LIST_DIR}/external)

#add_subdirectory(../)

set(HDR_LIST
  include/core/config.h
  include/core/messages.h
  include/core/instance_layout.h
  include/core/draw_intent.h
  include/core/render_system.h
  include/core/runtime.h
  include/core/assets_system.h
  include/core/global_ubo.h
  include/core/texture_set.h
  include/core/tile_map.h
  include/core/tile_batch.h
  include/core/actor_simulation.h
  include/core/brain_config_loader.h
  include/core/entity_scope.h
  include/core/script_environment.h
  include/core/world_scene_resource.h
  include/core/simulation.h
)

set(SRC_LIST
  main.cpp
  src/core/config.cpp
  src/core/app_config_resource.cpp
  src/core/render_system.cpp
  src/core/assets_system.cpp
  src/core/texture_set.cpp
  src/core/tile_map.cpp
  src/core/actor_simulation.cpp
  src/core/brain_config_loader.cpp
  src/core/script_environment.cpp
  src/core/world_scene_resource.cpp
  src/core/simulation.cpp
)

add_executable(${PROJECT_NAME} ${SRC_LIST} ${HDR_LIST})
target_link_libraries(
  ${PROJECT_NAME}
  devils_engine::devils_plane
  devils_engine::mood
  devils_engine::prefab
  glm::glm
  devils_engine::options
  tavl
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp
)
set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")
target_include_directories(${PROJECT_NAME} PRIVATE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/core>")

# Snapshot round-trip smoke: сериализует РЕАЛЬНЫЕ компоненты tile_frontier (glm-адаптеры +
# межсущностные ссылки) через aesthetics::serial и сверяет. Заголовочная зависимость от
# компонентов -> линкует только devils_plane (тянет aesthetics/act/acumen/mood транзитивно).
add_executable(tile_frontier_snapshot_smoke snapshot_smoke.cpp)
target_link_libraries(tile_frontier_snapshot_smoke
  devils_engine::devils_plane
  devils_engine::prefab
  glm::glm
  tavl
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp
)
target_include_directories(tile_frontier_snapshot_smoke PRIVATE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/core>"
)
set_target_properties(tile_frontier_snapshot_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")

# Resume-детерминизм: прогоняет РЕАЛЬНУЮ симуляцию слайса, save->load в чистый слайс, гоняет оба
# синхронно и сверяет состояние побайтово через dump_world. Линкует actor_simulation.cpp (импл
# слайса + регистрация компонентов через actor_snapshot.h) поверх devils_plane.
set(TILE_FRONTIER_BRAIN_TEST_SOURCES
  src/core/actor_simulation.cpp
  src/core/brain_config_loader.cpp
  src/core/script_environment.cpp
  test_brain_fixture.cpp
  test_brain_fixture.h
)

add_executable(tile_frontier_resume_smoke resume_smoke.cpp ${TILE_FRONTIER_BRAIN_TEST_SOURCES})
target_link_libraries(tile_frontier_resume_smoke
  devils_engine::devils_plane
  devils_engine::demiurg
  devils_engine::prefab
  glm::glm
  tavl
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp
)
target_include_directories(tile_frontier_resume_smoke PRIVATE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/core>"
)
target_compile_definitions(tile_frontier_resume_smoke PRIVATE
  TILE_FRONTIER_SOURCE_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources/modules/")
set_target_properties(tile_frontier_resume_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")

# Full actor-pipeline scaling benchmark. Not registered in ctest: it is an explicit performance run,
# but it fails non-zero unless 1/2/4/8 workers produce the same world hash and serialized bytes.
add_executable(tile_frontier_mt_benchmark mt_benchmark.cpp ${TILE_FRONTIER_BRAIN_TEST_SOURCES})
target_link_libraries(tile_frontier_mt_benchmark
  devils_engine::devils_plane
  devils_engine::demiurg
  devils_engine::prefab
  glm::glm
  tavl
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp
)
target_include_directories(tile_frontier_mt_benchmark PRIVATE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/core>"
)
target_compile_definitions(tile_frontier_mt_benchmark PRIVATE
  TILE_FRONTIER_SOURCE_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources/modules/")
set_target_properties(tile_frontier_mt_benchmark PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")

# Config-loaded void action acceptance: tavl -> devils_script -> act::script_function<void> ->
# catalogue deferred record/commit. Kept headless and independent of the application/render stack.
add_executable(tile_frontier_config_effect_smoke
  config_effect_smoke.cpp
  ${TILE_FRONTIER_BRAIN_TEST_SOURCES}
)
target_link_libraries(tile_frontier_config_effect_smoke
  devils_engine::devils_plane
  devils_engine::prefab
  devils_engine::demiurg
  glm::glm
  tavl
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp
)
target_include_directories(tile_frontier_config_effect_smoke PRIVATE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/core>"
)
target_compile_definitions(tile_frontier_config_effect_smoke PRIVATE
  TILE_FRONTIER_SOURCE_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources/modules/")
set_target_properties(tile_frontier_config_effect_smoke PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")

# Регистрация в ctest под меткой devils_engine (ctest -L devils_engine). Плоский main → код
# возврата 0/1 = pass/fail, без doctest-дискавери. Метка ставит их в один прогон с остальными.
if (DEVILS_ENGINE_BUILD_TESTS)
  add_test(NAME tile_frontier_snapshot_smoke COMMAND tile_frontier_snapshot_smoke)
  set_tests_properties(tile_frontier_snapshot_smoke PROPERTIES LABELS devils_engine)
  add_test(NAME tile_frontier_resume_smoke COMMAND tile_frontier_resume_smoke)
  set_tests_properties(tile_frontier_resume_smoke PROPERTIES LABELS devils_engine)
  add_test(NAME tile_frontier_config_effect_smoke COMMAND tile_frontier_config_effect_smoke)
  set_tests_properties(tile_frontier_config_effect_smoke PROPERTIES LABELS devils_engine)
endif()
include("${CMAKE_SOURCE_DIR}/cmake/devils_portable_runtime.cmake")
devils_bundle_runtime_dependencies(${PROJECT_NAME})

if (WIN32)
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}>
      $<TARGET_FILE_DIR:${PROJECT_NAME}>
    COMMAND_EXPAND_LISTS
  )
endif()

# Базовые движковые ресурсы: default config, общие шейдеры и прочее, что не должно жить в проекте.
# Проектные resources/ копируются ниже и могут точечно перекрыть дефолты.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  # Миграция UI из non-replaceable engine registry в modules/core. copy_directory сам не удаляет
  # старые файлы, поэтому без этой очистки прежний engine/ui/entry.lua остается в build output.
  COMMAND ${CMAKE_COMMAND} -E rm -rf
    ${PROJECT_OUTPUT_DIRECTORY}/resources/engine/ui
  COMMAND ${CMAKE_COMMAND} -E rm -rf
    ${PROJECT_OUTPUT_DIRECTORY}/resources/modules/core/ui_states
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/resources
    ${PROJECT_OUTPUT_DIRECTORY}/resources
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_LIST_DIR}/resources
    ${PROJECT_OUTPUT_DIRECTORY}/resources
)

# UI scripts are ordinary overridable module resources. The first module in the active module list
# has the highest priority, just like startup/entry and states/*.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_LIST_DIR}/resources/ui
    ${PROJECT_OUTPUT_DIRECTORY}/resources/modules/core/ui
)

# render-graph описание грузится через движковый demiurg-реестр (resources/engine/render_config/),
# а не сканом папки. test_render_config остаётся каноничным источником (шарится с fast_test/tests),
# копируем его в движковое дерево ПОСЛЕ копирования resources/ (порядок POST_BUILD сохраняется).
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/subprojects/tests/test_render_config
    ${PROJECT_OUTPUT_DIRECTORY}/resources/engine/render_config
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/subprojects/tests/shaders
    ${PROJECT_OUTPUT_DIRECTORY}/shaders
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/subprojects/tests/shaders
    ${PROJECT_OUTPUT_DIRECTORY}/tests/shaders
)

# Шейдеры через движковый demiurg-реестр (Фаза 1): resources/engine/shaders/*.glsl (тип "shaders").
# Копии выше (shaders/, tests/shaders/) держит fs-fallback painter'а и корневой main.cpp — не трогаем.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/subprojects/tests/shaders
    ${PROJECT_OUTPUT_DIRECTORY}/resources/engine/shaders
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/resources/engine/shaders
    ${PROJECT_OUTPUT_DIRECTORY}/resources/engine/shaders
)
