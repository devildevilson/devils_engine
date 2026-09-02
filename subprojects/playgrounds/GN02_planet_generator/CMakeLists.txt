cmake_minimum_required(VERSION 3.30)

project(GN02_planet_generator VERSION 0.1.0 LANGUAGES CXX)

# Площадка headless по сути: результат генератора — данные, и все проверки идут числами. Просмотрщик
# живёт в том же исполняемом файле не ради экономии, а потому что смена шага генерации — это НОВЫЙ
# прогон генератора: разносить их по двум программам значило бы дублировать сборку пайплайна.
add_executable(GN02_planet_generator
  src/main.cpp
  src/names.cpp src/names.h
  src/package.cpp src/package.h
  src/planet_tools.cpp src/planet_tools.h
  src/visual.cpp src/visual.h
  src/viewer.cpp src/viewer.h)

target_compile_definitions(GN02_planet_generator PRIVATE
  GN02_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources"
  PLAYGROUND_COMMON_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../common/resources")

target_link_libraries(GN02_planet_generator PRIVATE
  devils_engine::originator
  devils_engine::originator_lua
  devils_engine::originator_primitives
  devils_engine::playground_common
  devils_engine::painter
  devils_engine::input
  devils_engine::visage
  devils_engine::utils
  devils_engine::options
  glm::glm
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp)

set_target_properties(GN02_planet_generator PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/GN02_planet_generator/bin")

if (DEVILS_ENGINE_BUILD_TESTS)
  # Разрешение проверки — 49152 клетки, и это НИЖНЯЯ граница модели, а не выбор ради скорости.
  # Океанические острова здесь физического происхождения: цепь горячей точки и вулканы островной дуги.
  # Постройка поперёк в две сотни километров на решётке из 16 тысяч клеток занимает полторы клетки, то
  # есть не существует, и проверка «острова годятся под провинцию» падала честно; на 32768 она падает
  # на части зёрен. Полное разрешение (262144 по умолчанию) остаётся ручным прогоном.
  add_test(NAME GN02_planet_generator_verify
    COMMAND GN02_planet_generator --verify --cells=49152 --provinces=450 --sea-zones=50 --cultures=24)
  set_tests_properties(GN02_planet_generator_verify PROPERTIES LABELS devils_engine)
endif()
