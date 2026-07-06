cmake_minimum_required(VERSION 3.30)

project(
  tile_frontier
  VERSION 0.1.1
  LANGUAGES CXX C
)

set(PROJECT_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}")

set(PROJECT_EXTERNAL_DEPENDENCY_DIR ${CMAKE_CURRENT_LIST_DIR}/external)

#add_subdirectory(../)

set(HDR_LIST
  include/core/actors.h
  include/core/config.h
  include/core/lua_script_resource.h
  include/core/messages.h
  include/core/message_dispatcher.h
  include/core/instance_layout.h
  include/core/draw_intent.h
  include/core/sound_system.h
  include/core/render_system.h
  include/core/assets_system.h
  include/core/global_ubo.h
  include/core/texture_set.h
  include/core/tile_map.h
  include/core/tile_batch.h
  include/core/actor_simulation.h
  include/core/simulation.h
)

set(SRC_LIST
  main.cpp
  src/core/config.cpp
  src/core/app_config_resource.cpp
  src/core/lua_script_resource.cpp
  src/core/sound_system.cpp
  src/core/render_system.cpp
  src/core/assets_system.cpp
  src/core/texture_set.cpp
  src/core/tile_map.cpp
  src/core/actor_simulation.cpp
  src/core/simulation.cpp
)

add_executable(${PROJECT_NAME} ${SRC_LIST} ${HDR_LIST})
target_link_libraries(
  ${PROJECT_NAME}
  devils_engine::devils_plane
  devils_engine::mood
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
add_executable(tile_frontier_resume_smoke resume_smoke.cpp src/core/actor_simulation.cpp)
target_link_libraries(tile_frontier_resume_smoke
  devils_engine::devils_plane
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
set_target_properties(tile_frontier_resume_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")

# Регистрация в ctest под меткой devils_engine (ctest -L devils_engine). Плоский main → код
# возврата 0/1 = pass/fail, без doctest-дискавери. Метка ставит их в один прогон с остальными.
if (DEVILS_ENGINE_BUILD_TESTS)
  add_test(NAME tile_frontier_snapshot_smoke COMMAND tile_frontier_snapshot_smoke)
  set_tests_properties(tile_frontier_snapshot_smoke PROPERTIES LABELS devils_engine)
  add_test(NAME tile_frontier_resume_smoke COMMAND tile_frontier_resume_smoke)
  set_tests_properties(tile_frontier_resume_smoke PROPERTIES LABELS devils_engine)
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

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory 
    ${CMAKE_CURRENT_LIST_DIR}/resources 
    ${PROJECT_OUTPUT_DIRECTORY}/resources
)

# UI lua scripts are also exposed through the engine demiurg registry as resources/engine/ui/*.lua.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_LIST_DIR}/resources/ui
    ${PROJECT_OUTPUT_DIRECTORY}/resources/engine/ui
)

# render-graph описание грузится через движковый demiurg-реестр (resources/engine/render_config/),
# а не сканом папки. test_render_config остаётся каноничным источником (шарится с fast_test/tests),
# копируем его в движковое дерево ПОСЛЕ копирования resources/ (порядок POST_BUILD сохраняется).
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/tests/test_render_config
    ${PROJECT_OUTPUT_DIRECTORY}/resources/engine/render_config
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/tests/shaders
    ${PROJECT_OUTPUT_DIRECTORY}/shaders
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/tests/shaders
    ${PROJECT_OUTPUT_DIRECTORY}/tests/shaders
)

# Шейдеры через движковый demiurg-реестр (Фаза 1): resources/engine/shaders/*.glsl (тип "shaders").
# Копии выше (shaders/, tests/shaders/) держит fs-fallback painter'а и корневой main.cpp — не трогаем.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/tests/shaders
    ${PROJECT_OUTPUT_DIRECTORY}/resources/engine/shaders
)
