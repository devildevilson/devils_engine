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
  include/core/world_rules.h
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
  src/core/world_rules.cpp
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
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>")

# ─────────────────────────────────────────────────────────────────────────────────────────────
# Сетевой слой. Отдельная цель, а не часть причинного ядра, и это тот же принцип, что и раскол
# выше: транспорт НЕ определяет состояние мира. Причинное ядро обязано собираться и считать без
# единого сокета — и собирается, потому что `frontier_causal` о существовании этой цели не знает.
#
# Существует только там, где есть настоящий бэкенд: GNS-адаптер собирается по флагу.
# ─────────────────────────────────────────────────────────────────────────────────────────────

if (TARGET devils_engine::network_gns)
  set(NET_HDR_LIST
    include/net/actor_component_wire.h
    include/net/player_intent_wire.h
    include/net/link.h
    include/net/protocol.h
    include/net/canonical_action.h
    include/net/authority.h
    include/net/follower.h
    include/net/live_runtime.h
    include/net/network_settings.h
  )
  set(NET_SRC_LIST
    src/net/link.cpp
    src/net/protocol.cpp
    src/net/authority.cpp
    src/net/follower.cpp
    src/net/live_runtime.cpp
    src/net/network_settings.cpp
    # Клиентская оркестровка входа в сессию: без презентации, поэтому живёт здесь и проверяется
    # тестом без окна (frontier_rejoin_smoke).
    src/core/net_client.cpp
  )
  list(APPEND NET_HDR_LIST include/core/net_client.h)
  add_library(frontier_net STATIC ${NET_SRC_LIST} ${NET_HDR_LIST})
  target_compile_definitions(frontier_net PUBLIC FRONTIER_ONLINE_HAS_NETWORK=1)
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
  include/core/broker.h
  include/core/app_config_resource.h
  include/core/launch_options.h
  include/core/actor_batch.h
  include/core/render_system.h
  include/core/runtime.h
  include/core/assets_system.h
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
  src/core/launch_options.cpp
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
# Клиент присоединяется к живой сессии (`--join`) тем же ведомым, что headless-процесс.
if (TARGET frontier_net)
  target_link_libraries(${PROJECT_NAME} frontier_net)
else()
  # Без GNS клиент собирает заглушку net_client (ветка без FRONTIER_ONLINE_HAS_NETWORK).
  target_sources(${PROJECT_NAME} PRIVATE src/core/net_client.cpp include/core/net_client.h)
endif()

# ─────────────────────────────────────────────────────────────────────────────────────────────
# Headless-авторитет. Линкует ТОЛЬКО причинное ядро: ни одной графической библиотеки в списке.
# Если этот таргет собрался — раскол настоящий.
# ─────────────────────────────────────────────────────────────────────────────────────────────

add_executable(frontier_online_server server_main.cpp)
target_link_libraries(frontier_online_server frontier_causal)
if (TARGET frontier_net)
  target_link_libraries(frontier_online_server frontier_net)
endif()
set_target_properties(frontier_online_server PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")
if (UNIX AND NOT APPLE)
  target_link_options(frontier_online_server PRIVATE -Wl,--as-needed)
endif()
# On this build host, libc injects an x86-64-v3 *needed* ELF note even into
# baseline code. NETLAB01 measured the same issue and drops that note for its
# baseline artifact. Keep the AVX build's note: that build really requires AVX.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64" AND
    DEVILS_ENGINE_ARCH STREQUAL "OFF" AND CMAKE_OBJCOPY)
  add_custom_command(TARGET frontier_online_server POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} --remove-section=.note.gnu.property
            $<TARGET_FILE:frontier_online_server>
    COMMENT "Dropping the build host's inherited x86 ISA-level marker")
endif()

# Packages for other machines. The headless package is deliberately separate from the graphical
# client's resource copy. Both Linux roles use this one executable; the Windows build of the same
# target supplies the third machine's executable. Each packaging script starts from an empty
# staging tree and carries the shared libraries along (cmake/package_runtime.cmake).
include("${CMAKE_SOURCE_DIR}/cmake/devils_portable_runtime.cmake")
devils_set_local_rpath(frontier_online_server)
set(FRONTIER_SYSTEM_RUNTIME_LIBS "")
if (MSVC)
  # Only the list of MSVC runtime DLLs is wanted, not install() rules.
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
  include(InstallRequiredSystemLibraries)
  list(JOIN CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS "|" FRONTIER_SYSTEM_RUNTIME_LIBS)
endif()

add_custom_target(frontier_online_headless_package
  COMMAND ${CMAKE_COMMAND}
    "-DBINARY=$<TARGET_FILE:frontier_online_server>"
    "-DRESOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/resources/modules"
    "-DPACKAGE_DIR=${PROJECT_OUTPUT_DIRECTORY}/headless-package"
    "-DBUILD_BIN_DIR=$<TARGET_FILE_DIR:frontier_online_server>"
    "-DSYSTEM_RUNTIME_LIBS=${FRONTIER_SYSTEM_RUNTIME_LIBS}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_headless.cmake"
  DEPENDS frontier_online_server
  VERBATIM)

# The graphical client with the resource tree that frontier_online_resources syncs (end of file).
add_custom_target(frontier_online_client_package
  COMMAND ${CMAKE_COMMAND}
    "-DBINARY=$<TARGET_FILE:${PROJECT_NAME}>"
    "-DBUILD_OUTPUT=${PROJECT_OUTPUT_DIRECTORY}"
    "-DPACKAGE_DIR=${PROJECT_OUTPUT_DIRECTORY}/client-package"
    "-DBUILD_BIN_DIR=$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    "-DSYSTEM_RUNTIME_LIBS=${FRONTIER_SYSTEM_RUNTIME_LIBS}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_client.cmake"
  DEPENDS ${PROJECT_NAME}
  VERBATIM)

# Продукт: клиент и headless-сервер рядом в bin/, общий resources/ (см. cmake/package_product.cmake). Локально обе
# части из одной сборки; в CI клиент берётся из AVX-сборки, сервер — из базовой.
add_custom_target(frontier_online_package
  COMMAND ${CMAKE_COMMAND}
    "-DCLIENT_PACKAGE=${PROJECT_OUTPUT_DIRECTORY}/client-package"
    "-DSERVER_PACKAGE=${PROJECT_OUTPUT_DIRECTORY}/headless-package"
    "-DPACKAGE_DIR=${PROJECT_OUTPUT_DIRECTORY}/package"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_product.cmake"
  VERBATIM)
add_dependencies(frontier_online_package frontier_online_client_package frontier_online_headless_package)

# ─────────────────────────────────────────────────────────────────────────────────────────────
# Тесты форка: детерминизм (resume, time, tick, reproducible), земля, присоединение, живая сессия
# в одном процессе и в трёх, враждебные участники. Стенды tile_frontier, закрепляющие неизменные
# здесь контракты, не форкались и остаются у родителя. Цели и их регистрация — под одним условием.
# ─────────────────────────────────────────────────────────────────────────────────────────────
if (DEVILS_ENGINE_BUILD_TESTS)

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
  set_target_properties(frontier_resume_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")

  if (TARGET frontier_net)
    add_executable(frontier_join_smoke join_smoke.cpp)
    target_link_libraries(frontier_join_smoke frontier_net)
    set_target_properties(frontier_join_smoke PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")

    add_executable(frontier_live_tick_smoke live_tick_smoke.cpp)
    target_link_libraries(frontier_live_tick_smoke frontier_net)
    set_target_properties(frontier_live_tick_smoke PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")

    # Повторный вход графического клиента: обрыв, новый авторитет на том же порту, shutdown.
    add_executable(frontier_rejoin_smoke rejoin_smoke.cpp)
    target_link_libraries(frontier_rejoin_smoke frontier_net)
    set_target_properties(frontier_rejoin_smoke PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")

    # Враждебные участники и замолчавший авторитет: слоты, сроки, нарушения, квоты, история.
    add_executable(frontier_hardening_smoke hardening_smoke.cpp)
    target_link_libraries(frontier_hardening_smoke frontier_net)
    set_target_properties(frontier_hardening_smoke PROPERTIES
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
  set_target_properties(frontier_time_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")

  # Отбор тайлов по объединению камер и сетка мира сессии: чистые функции клиента.
  add_executable(frontier_tile_view_smoke tile_view_smoke.cpp src/core/tile_map.cpp)
  # Камера — движковый тип (simul/camera.h): презентационная сторона, не причинное ядро.
  target_link_libraries(frontier_tile_view_smoke frontier_causal devils_engine::simul glm::glm)
  set_target_properties(frontier_tile_view_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")

  add_executable(frontier_tick_smoke tick_smoke.cpp)
  target_link_libraries(frontier_tick_smoke frontier_causal)
  set_target_properties(frontier_tick_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")

  devils_add_test(NAME frontier_resume_smoke COMMAND frontier_resume_smoke)
  set_tests_properties(frontier_resume_smoke PROPERTIES LABELS devils_engine)
  devils_add_test(NAME frontier_time_smoke COMMAND frontier_time_smoke)
  set_tests_properties(frontier_time_smoke PROPERTIES LABELS devils_engine)
  devils_add_test(NAME frontier_tile_view_smoke COMMAND frontier_tile_view_smoke)
  set_tests_properties(frontier_tile_view_smoke PROPERTIES LABELS devils_engine)
  devils_add_test(NAME frontier_tick_smoke COMMAND frontier_tick_smoke)
  set_tests_properties(frontier_tick_smoke PROPERTIES LABELS devils_engine TIMEOUT 180)
  # Авторитет обязан дойти до объявленного тика и напечатать корень. Он же — проверка раскола:
  # цель линкуется без графических библиотек, поэтому её падение здесь означает, что причинное
  # ядро снова чего-то от презентации захотело.
  devils_add_test(NAME frontier_server_headless COMMAND frontier_online_server --ticks=120 --verify)
  set_tests_properties(frontier_server_headless PROPERTIES LABELS devils_engine)
  # Земля: считается ли она без окна и текстур, не зависит ли от ПОРЯДКА обхода чанков и не
  # выродилась ли в один класс. Первые два — контракт «чанк = функция (зерно, ключ)», без которого
  # нельзя считать землю на обеих сторонах вместо того, чтобы гонять её по сети. Третье ловит
  # сорванную калибровку: генератор отработал, а мира нет.
  devils_add_test(NAME frontier_terrain COMMAND frontier_online_server --terrain=8 --world-seed=20260911)
  set_tests_properties(frontier_terrain PROPERTIES LABELS devils_engine)
  # Присоединение: настоящий транспорт, настоящее рукопожатие, сверка причинного корня двух
  # независимо загруженных миров — и отказ С ПРИЧИНОЙ на двух разных полях совместимости.
  if (TARGET frontier_net)
    devils_add_test(NAME frontier_join_smoke COMMAND frontier_join_smoke)
    set_tests_properties(frontier_join_smoke PROPERTIES LABELS devils_engine TIMEOUT 180)
    devils_add_test(NAME frontier_live_tick_smoke COMMAND frontier_live_tick_smoke)
    set_tests_properties(frontier_live_tick_smoke PROPERTIES LABELS devils_engine TIMEOUT 180)
    devils_add_test(NAME frontier_live_tick_1024_smoke COMMAND frontier_live_tick_smoke --actors=1024)
    set_tests_properties(frontier_live_tick_1024_smoke PROPERTIES LABELS devils_engine TIMEOUT 180)
    devils_add_test(NAME frontier_rejoin_smoke COMMAND frontier_rejoin_smoke)
    set_tests_properties(frontier_rejoin_smoke PROPERTIES LABELS devils_engine TIMEOUT 120)
    devils_add_test(NAME frontier_hardening_smoke COMMAND frontier_hardening_smoke)
    set_tests_properties(frontier_hardening_smoke PROPERTIES LABELS devils_engine TIMEOUT 180)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if (Python3_Interpreter_FOUND)
      devils_add_test(NAME frontier_live_process_smoke
        COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_live_process_smoke.py"
          $<TARGET_FILE:frontier_online_server>)
      set_tests_properties(frontier_live_process_smoke PROPERTIES LABELS devils_engine TIMEOUT 45)
      devils_add_test(NAME frontier_live_process_1024_smoke
        COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_live_process_smoke.py"
          $<TARGET_FILE:frontier_online_server> 1024)
      set_tests_properties(frontier_live_process_1024_smoke PROPERTIES LABELS devils_engine TIMEOUT 90)
      # Режимы командной строки: --break-compat (отказ с причиной) и --play (уход участника штатен).
      devils_add_test(NAME frontier_session_cli_smoke
        COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_session_cli_smoke.py"
          $<TARGET_FILE:frontier_online_server>)
      set_tests_properties(frontier_session_cli_smoke PROPERTIES LABELS devils_engine TIMEOUT 120)
    endif()
  endif()
  # Два одинаковых прогона и 1/2/4/8 потоков обязаны дать один причинный корень. Формулировка
  # нарочно слабая: именно она падала, пока полоса scratch выбиралась по потоку ОС.
  devils_add_test(NAME frontier_server_reproducible
    COMMAND ${CMAKE_COMMAND}
      -DFRONTIER_SERVER=$<TARGET_FILE:frontier_online_server>
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_reproducible.cmake")
  set_tests_properties(frontier_server_reproducible PROPERTIES LABELS devils_engine)
endif()

devils_bundle_runtime_dependencies(${PROJECT_NAME})

if (WIN32)
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E $<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:${PROJECT_NAME}>>,copy_if_different,true>
      $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}>
      $<TARGET_FILE_DIR:${PROJECT_NAME}>
    COMMAND_EXPAND_LISTS
  )
endif()

# Ресурсы рядом с бинарниками (клиент, сервер и тесты ищут их от папки проекта). Отдельная цель, а
# не POST_BUILD клиента: POST_BUILD повторяется только при перелинковке, и правка ресурса без правки
# кода оставляла старые файлы, а copy_directory не удаляет то, что из исходников убрали. Устаревший
# файл в причинном каталоге меняет content_root клиента против headless-пакета (тот копируется из
# исходников начисто) — рукопожатие отказало бы с content mismatch. Поэтому синхронизация начисто на
# каждой сборке. Порядок копий — порядок перекрытия: движковые умолчания, затем проектные ресурсы.
set(FRONTIER_ONLINE_RESOURCE_OUT "${PROJECT_OUTPUT_DIRECTORY}/resources")
# Штамп лежит внутри синхронизируемого каталога: удалили каталог руками — синхронизация повторится.
set(FRONTIER_ONLINE_RESOURCE_STAMP "${FRONTIER_ONLINE_RESOURCE_OUT}/.frontier_online_synced")
file(GLOB_RECURSE FRONTIER_ONLINE_RESOURCE_INPUTS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/resources/engine/shaders/*"
  "${CMAKE_CURRENT_LIST_DIR}/resources/*"
  "${CMAKE_SOURCE_DIR}/subprojects/tests/test_render_config/*"
  "${CMAKE_SOURCE_DIR}/subprojects/tests/shaders/*")
# Список входов — тоже зависимость: удалённый из исходников файл не делает штамп устаревшим (его
# время больше не сравнивается), а этот файл переписывается, только когда список изменился.
list(JOIN FRONTIER_ONLINE_RESOURCE_INPUTS "\n" FRONTIER_ONLINE_RESOURCE_LIST)
set(FRONTIER_ONLINE_RESOURCE_LIST_FILE "${CMAKE_CURRENT_BINARY_DIR}/frontier_online_resources.list")
file(CONFIGURE OUTPUT "${FRONTIER_ONLINE_RESOURCE_LIST_FILE}" CONTENT "${FRONTIER_ONLINE_RESOURCE_LIST}")
add_custom_command(OUTPUT "${FRONTIER_ONLINE_RESOURCE_STAMP}"
  # shaders/ и tests/shaders/ в корне — следы прежних копий (запасной путь painter без реестра);
  # проект берёт шейдеры только из реестра движка, их удаление здесь убирает и старые сборки.
  COMMAND ${CMAKE_COMMAND} -E rm -rf
    "${FRONTIER_ONLINE_RESOURCE_OUT}" "${PROJECT_OUTPUT_DIRECTORY}/shaders"
    "${PROJECT_OUTPUT_DIRECTORY}/tests"
  # resources/modules (и лицензии) — ресурсы проекта.
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_LIST_DIR}/resources" "${FRONTIER_ONLINE_RESOURCE_OUT}"
  # resources/engine — неизменяемое состояние движка в проекте: рендер-граф и шейдеры. Движковые
  # config/* (предварительные числа для тестов и площадок) проекту не нужны: топологию и правила
  # мира он задаёт в коде (runtime_bootstrap, world_rules.h), настройки игрока лежат в корне.
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/subprojects/tests/test_render_config"
    "${FRONTIER_ONLINE_RESOURCE_OUT}/engine/render_config"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/subprojects/tests/shaders" "${FRONTIER_ONLINE_RESOURCE_OUT}/engine/shaders"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/resources/engine/shaders" "${FRONTIER_ONLINE_RESOURCE_OUT}/engine/shaders"
  COMMAND ${CMAKE_COMMAND} -E touch "${FRONTIER_ONLINE_RESOURCE_STAMP}"
  DEPENDS ${FRONTIER_ONLINE_RESOURCE_INPUTS} "${FRONTIER_ONLINE_RESOURCE_LIST_FILE}"
  COMMENT "frontier_online: sync resources next to the binaries"
  VERBATIM)
add_custom_target(frontier_online_resources DEPENDS "${FRONTIER_ONLINE_RESOURCE_STAMP}")
add_dependencies(${PROJECT_NAME} frontier_online_resources)
add_dependencies(frontier_online_server frontier_online_resources)
foreach(resource_user frontier_resume_smoke frontier_time_smoke frontier_tick_smoke
        frontier_join_smoke frontier_live_tick_smoke frontier_hardening_smoke frontier_rejoin_smoke)
  if (TARGET ${resource_user})
    add_dependencies(${resource_user} frontier_online_resources)
  endif()
endforeach()
