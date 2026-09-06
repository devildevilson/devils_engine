cmake_minimum_required(VERSION 3.30)

project(
  devils_originator
  VERSION 0.1.0
  LANGUAGES CXX
)

add_library(devils_originator
  src/originator/common.cpp
  src/originator/buffer.cpp
  src/originator/tools.cpp
  src/originator/standard_tools.cpp
  src/originator/scatter_tools.cpp
  src/originator/constraint_tools.cpp
  src/originator/volume_tools.cpp
  src/originator/graph_tools.cpp
  src/originator/pipeline.cpp
  src/originator/computation_queue.cpp
  src/originator/execution_profile.cpp
  src/originator/device_form.cpp
  include/devils_engine/originator/common.h
  include/devils_engine/originator/buffer.h
  include/devils_engine/originator/tools.h
  include/devils_engine/originator/pipeline.h
  include/devils_engine/originator/computation_queue.h
  include/devils_engine/originator/execution_profile.h
  include/devils_engine/originator/device_form.h
)

target_include_directories(devils_originator
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
  PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/originator>"
)

target_link_libraries(devils_originator
  PUBLIC devils_engine::options devils_engine::utils
  PRIVATE tavl
)

add_library(devils_engine::originator ALIAS devils_originator)

# Слой demiurg: генератор как РЕСУРС. Отдельная цель по той же причине, что lua и devils_script —
# ядру генератора незачем знать, откуда приехал текст. Потребителю, который читает документы с диска
# (стенд, тест), эта цель не нужна вовсе.
add_library(devils_originator_config
  src/originator/generator_resource.cpp
  include/devils_engine/originator/generator_resource.h
)

target_include_directories(devils_originator_config
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
)

target_link_libraries(devils_originator_config
  PUBLIC devils_engine::originator devils_engine::demiurg
  PRIVATE devils_engine::options devils_engine::utils
)

add_library(devils_engine::originator_config ALIAS devils_originator_config)

# Слой devils_script — тоже отдельная цель по той же причине, что и lua: ядро генератора не обязано
# тянуть за собой ни один скриптовый backend.
add_library(devils_originator_script
  src/originator/script_program.cpp
  src/originator/script_translate.cpp
  include/devils_engine/originator/script_program.h
  include/devils_engine/originator/script_translate.h
)

target_include_directories(devils_originator_script
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
)

target_link_libraries(devils_originator_script
  PUBLIC devils_engine::originator
  # tavl — тот же парсер, которым разбирает ds: транслятор идёт по AST, который отдаёт сам ds, и
  # второго разбора текста в проекте не появляется.
  PRIVATE devils_engine::options devils_engine::utils devils_script tavl
)

add_library(devils_engine::originator_script ALIAS devils_originator_script)

# Нативные примитивы поверх сторонних библиотек: FastNoise2 (поля шума) и jc_voronoi (топология
# областей). Отдельная цель по той же причине — ядро не обязано тянуть внешние зависимости.
add_library(devils_originator_primitives
  src/originator/noise_tools.cpp
  src/originator/voronoi_tools.cpp
  include/devils_engine/originator/primitives.h
)

target_include_directories(devils_originator_primitives
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
)

target_link_libraries(devils_originator_primitives
  PUBLIC devils_engine::originator
  PRIVATE devils_engine::options devils_engine::utils FastNoise jc_voronoi::jc_voronoi
)

add_library(devils_engine::originator_primitives ALIAS devils_originator_primitives)

# Устройственный слой: originator СОСТАВЛЯЕТ pipeline для вычислений на GPU из своей же очереди.
# Отдельная цель по той же причине, что lua и devils_script — ядру генератора незачем знать про
# Vulkan, а потребителю, которому нужен только путь на CPU, незачем линковать painter.
add_library(devils_originator_device
  src/originator/device_queue.cpp
  src/originator/device_executor.cpp
  include/devils_engine/originator/device_queue.h
  include/devils_engine/originator/device_executor.h
)

target_include_directories(devils_originator_device
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
)

target_link_libraries(devils_originator_device
  PUBLIC devils_engine::originator devils_engine::painter
  PRIVATE devils_engine::options devils_engine::utils
)

add_library(devils_engine::originator_device ALIAS devils_originator_device)

# Скриптовый слой — ОТДЕЛЬНАЯ цель: ядро генератора не должно тянуть lua за собой, а потребителю,
# которому нужен только пайплайн на C++, незачем линковать интерпретатор.
if (MSVC)
  set(DEVILS_ORIGINATOR_LUA_LIBRARIES lua54)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(originator_lua54 REQUIRED IMPORTED_TARGET lua5.4)
  set(DEVILS_ORIGINATOR_LUA_LIBRARIES PkgConfig::originator_lua54)
endif()

add_library(devils_originator_lua
  src/originator/script_host.cpp
  include/devils_engine/originator/script_host.h
)

target_include_directories(devils_originator_lua
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
)

target_link_libraries(devils_originator_lua
  PUBLIC devils_engine::originator devils_engine::originator_script sol2::sol2 ${DEVILS_ORIGINATOR_LUA_LIBRARIES}
  # Базовые функции lua приходят из движка, а не пишутся заново: `base.prng64_2`, `base.value`,
  # `base.dice` и упаковка сделаны на общих утилитах, поэтому скрипт генератора и остальной движок
  # считают ОДНИМ хешем. Цель `bindings_base` намеренно без интерфейса — генератор headless.
  PRIVATE devils_engine::options devils_engine::utils devils_engine::bindings_base
)

if (WIN32)
  target_link_directories(devils_originator_lua PUBLIC ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/lua54)
  target_include_directories(devils_originator_lua PRIVATE ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/lua54/include)
endif()

add_library(devils_engine::originator_lua ALIAS devils_originator_lua)
