cmake_minimum_required(VERSION 3.30)

project(GN02_planet_generator VERSION 0.1.0 LANGUAGES CXX)

add_executable(GN02_planet_generator
  src/main.cpp
  src/package.cpp
  src/planet_tools.cpp)

target_compile_definitions(GN02_planet_generator PRIVATE
  GN02_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources")

target_link_libraries(GN02_planet_generator PRIVATE
  devils_engine::originator
  devils_engine::originator_lua
  devils_engine::originator_primitives
  devils_engine::utils
  devils_engine::options)

set_target_properties(GN02_planet_generator PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/GN02_planet_generator/bin")

if (DEVILS_ENGINE_BUILD_TESTS)
  # Проверки идут на маленькой планете: свойства, которые они проверяют, от разрешения не зависят, а
  # время прогона зависит. Полное разрешение остаётся ручным прогоном.
  add_test(NAME GN02_planet_generator_verify
    COMMAND GN02_planet_generator --verify --cells=16384 --plates=14 --provinces=220 --sea-zones=30 --cultures=16)
  set_tests_properties(GN02_planet_generator_verify PROPERTIES LABELS devils_engine)
endif()
