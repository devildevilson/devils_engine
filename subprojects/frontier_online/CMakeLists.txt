cmake_minimum_required(VERSION 3.30)

project(
  frontier_online
  VERSION 0.0.1
  LANGUAGES CXX C
)

set(PROJECT_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/${PROJECT_NAME}")

# ─────────────────────────────────────────────────────────────────────────────────────────────
# Раскол на два слоя — главное содержание первого среза (SERVER-01).
#
# `frontier_causal` — ПРИЧИННОЕ ядро. Всё, что определяет состояние мира и обязано быть
# одинаковым у авторитета и у клиента. Оно линкует ТОЛЬКО причинные библиотеки: ни painter, ни
# visage, ни sound, ни input, ни simul. Это не стилистическое пожелание, а проверяемое свойство:
# если презентационная зависимость просочится в причинный код, headless-авторитет перестанет
# собираться, и это выяснится на сборке, а не на продакшене.
#
# `devils_engine::devils_plane` тут не подходит именно поэтому: он агрегирует ВСЁ, включая
# painter/visage/sound/input. Причинный слой перечисляет свои библиотеки поимённо.
# ─────────────────────────────────────────────────────────────────────────────────────────────

set(CAUSAL_HDR_LIST
  include/core/color32.h
  include/core/terrain.h
  include/core/content_root.h
  include/core/causal_content.h
  include/core/actor_simulation.h
  include/core/actor_checkpoint.h
  include/core/actor_snapshot.h
  include/core/brain_config_loader.h
  include/core/entity_scope.h
  include/core/spawn_scope.h
  include/core/script_environment.h
)

set(CAUSAL_SRC_LIST
  src/core/terrain.cpp
  src/core/content_root.cpp
  src/core/causal_content.cpp
  src/core/actor_simulation.cpp
  src/core/actor_checkpoint.cpp
  src/core/brain_config_loader.cpp
  src/core/script_environment.cpp
)

add_library(frontier_causal STATIC ${CAUSAL_SRC_LIST} ${CAUSAL_HDR_LIST})
target_link_libraries(frontier_causal PUBLIC
  devils_engine::aesthetics
  devils_engine::act
  devils_engine::acumen
  devils_engine::mood
  devils_engine::catalogue
  devils_engine::demiurg
  devils_engine::network
  # Земля: генератор объявлен в конфиге (originator_config), тела шагов — lua (originator_lua),
  # шум и вороной — примитивы. Ни одна из этих целей не тянет презентацию: lua-хост генератора
  # headless по построению, поэтому земля законно живёт в ПРИЧИННОМ слое.
  devils_engine::originator
  devils_engine::originator_config
  devils_engine::originator_lua
  devils_engine::originator_primitives
  devils_engine::utils
  devils_engine::options
  devils_engine::prefab
  glm::glm
  tavl
)
target_include_directories(frontier_causal PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/core>")

# ─────────────────────────────────────────────────────────────────────────────────────────────
# Сетевой слой. Отдельная цель, а не часть причинного ядра, и это тот же принцип, что и раскол
# выше: транспорт НЕ определяет состояние мира. Причинное ядро обязано собираться и считать без
# единого сокета — и собирается, потому что `frontier_causal` о существовании этой цели не знает.
#
# Существует только там, где есть настоящий бэкенд: GNS-адаптер собирается по флагу.
# ─────────────────────────────────────────────────────────────────────────────────────────────

if (TARGET devils_engine::network_gns)
  set(NET_HDR_LIST
    include/net/link.h
    include/net/protocol.h
    include/net/authority.h
    include/net/follower.h
  )
  set(NET_SRC_LIST
    src/net/link.cpp
    src/net/protocol.cpp
    src/net/authority.cpp
    src/net/follower.cpp
  )
  add_library(frontier_net STATIC ${NET_SRC_LIST} ${NET_HDR_LIST})
  target_link_libraries(frontier_net PUBLIC frontier_causal devils_engine::network_gns)
  target_include_directories(frontier_net PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>")
endif()

# ─────────────────────────────────────────────────────────────────────────────────────────────
# Клиент: причинное ядро + презентация. Здесь и только здесь появляются painter/visage/sound.
# ─────────────────────────────────────────────────────────────────────────────────────────────

set(CLIENT_HDR_LIST
  include/core/config.h
  include/core/messages.h
  include/core/instance_layout.h
  include/core/draw_intent.h
  include/core/frame_capture.h
  include/core/actor_batch.h
  include/core/render_system.h
  include/core/runtime.h
  include/core/assets_system.h
  include/core/global_ubo.h
  include/core/texture_set.h
  include/core/tile_map.h
  include/core/tile_batch.h
  include/core/frontier_online_game.h
  include/core/world_scene_resource.h
  include/core/simulation.h
)

set(CLIENT_SRC_LIST
  main.cpp
  src/core/config.cpp
  src/core/app_config_resource.cpp
  src/core/actor_batch.cpp
  src/core/frame_capture.cpp
  src/core/render_system.cpp
  src/core/assets_system.cpp
  src/core/texture_set.cpp
  src/core/tile_map.cpp
  src/core/frontier_online_game.cpp
  src/core/world_scene_resource.cpp
  src/core/simulation.cpp
)

add_executable(${PROJECT_NAME} ${CLIENT_SRC_LIST} ${CLIENT_HDR_LIST})
target_link_libraries(
  ${PROJECT_NAME}
  frontier_causal
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

# ─────────────────────────────────────────────────────────────────────────────────────────────
# Headless-авторитет. Линкует ТОЛЬКО причинное ядро: ни одной графической библиотеки в списке.
# Если этот таргет собрался — раскол настоящий.
# ─────────────────────────────────────────────────────────────────────────────────────────────

add_executable(frontier_online_server server_main.cpp)
target_link_libraries(frontier_online_server frontier_causal)
if (TARGET frontier_net)
  target_link_libraries(frontier_online_server frontier_net)
  target_compile_definitions(frontier_online_server PRIVATE FRONTIER_ONLINE_HAS_NETWORK=1)
endif()
target_compile_definitions(frontier_online_server PRIVATE
  FRONTIER_ONLINE_SOURCE_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources/modules/")
set_target_properties(frontier_online_server PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")

# ─────────────────────────────────────────────────────────────────────────────────────────────
# Регрессионный минимум форка: два смока, охраняющие ДЕТЕРМИНИЗМ, на котором стоит вся сетевая
# работа. Остальные стенды tile_frontier сознательно не форкались — они закрепляют контракты,
# которые здесь не меняются, и остаются у родителя.
# ─────────────────────────────────────────────────────────────────────────────────────────────

set(FRONTIER_BRAIN_TEST_SOURCES
  test_brain_fixture.cpp
  test_brain_fixture.h
)

add_executable(frontier_resume_smoke resume_smoke.cpp src/core/actor_batch.cpp ${FRONTIER_BRAIN_TEST_SOURCES})
target_link_libraries(frontier_resume_smoke
  frontier_causal
  devils_engine::devils_plane
  devils_engine::demiurg
  devils_engine::prefab
  glm::glm
  tavl
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp
)
target_compile_definitions(frontier_resume_smoke PRIVATE
  FRONTIER_ONLINE_SOURCE_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources/modules/")
set_target_properties(frontier_resume_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")

if (TARGET frontier_net)
  add_executable(frontier_join_smoke join_smoke.cpp)
  target_link_libraries(frontier_join_smoke frontier_net)
  target_compile_definitions(frontier_join_smoke PRIVATE
    FRONTIER_ONLINE_SOURCE_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources/modules/")
  set_target_properties(frontier_join_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")
endif()

add_executable(frontier_time_smoke time_smoke.cpp src/core/actor_batch.cpp ${FRONTIER_BRAIN_TEST_SOURCES})
target_link_libraries(frontier_time_smoke
  frontier_causal
  devils_engine::devils_plane
  devils_engine::demiurg
  devils_engine::prefab
  glm::glm
  tavl
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp
)
target_compile_definitions(frontier_time_smoke PRIVATE
  FRONTIER_ONLINE_SOURCE_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources/modules/")
set_target_properties(frontier_time_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")

if (DEVILS_ENGINE_BUILD_TESTS)
  add_test(NAME frontier_resume_smoke COMMAND frontier_resume_smoke)
  set_tests_properties(frontier_resume_smoke PROPERTIES LABELS devils_engine)
  add_test(NAME frontier_time_smoke COMMAND frontier_time_smoke)
  set_tests_properties(frontier_time_smoke PROPERTIES LABELS devils_engine)
  # Авторитет обязан дойти до объявленного тика и напечатать корень. Он же — проверка раскола:
  # цель линкуется без графических библиотек, поэтому её падение здесь означает, что причинное
  # ядро снова чего-то от презентации захотело.
  add_test(NAME frontier_server_headless COMMAND frontier_online_server --ticks=120 --verify)
  set_tests_properties(frontier_server_headless PROPERTIES LABELS devils_engine)
  # Два одинаковых прогона обязаны дать один причинный корень. Формулировка нарочно слабая:
  # именно она падала, пока полоса scratch выбиралась по потоку ОС.
  # Земля: считается ли она без окна и текстур, не зависит ли от ПОРЯДКА обхода чанков и не
  # выродилась ли в один класс. Первые два — контракт «чанк = функция (зерно, ключ)», без которого
  # нельзя считать землю на обеих сторонах вместо того, чтобы гонять её по сети. Третье ловит
  # сорванную калибровку: генератор отработал, а мира нет.
  add_test(NAME frontier_terrain COMMAND frontier_online_server --terrain=8 --world-seed=20260911)
  set_tests_properties(frontier_terrain PROPERTIES LABELS devils_engine)
  # Присоединение: настоящий транспорт, настоящее рукопожатие, сверка причинного корня двух
  # независимо загруженных миров — и отказ С ПРИЧИНОЙ на двух разных полях совместимости.
  if (TARGET frontier_join_smoke)
    add_test(NAME frontier_join_smoke COMMAND frontier_join_smoke)
    set_tests_properties(frontier_join_smoke PROPERTIES LABELS devils_engine TIMEOUT 180)
  endif()
  add_test(NAME frontier_server_reproducible
    COMMAND ${CMAKE_COMMAND}
      -DFRONTIER_SERVER=$<TARGET_FILE:frontier_online_server>
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_reproducible.cmake")
  set_tests_properties(frontier_server_reproducible PROPERTIES LABELS devils_engine)
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

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_LIST_DIR}/resources/ui
    ${PROJECT_OUTPUT_DIRECTORY}/resources/modules/core/ui
)

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
