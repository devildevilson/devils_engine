cmake_minimum_required(VERSION 3.30)

project(cardgame VERSION 0.0.1 LANGUAGES CXX)

add_library(cardgame_core STATIC
  include/cardgame/combat.h
  src/combat.cpp
)
add_library(cardgame::core ALIAS cardgame_core)
target_link_libraries(cardgame_core PUBLIC devils_engine::simul devils_engine::options)
target_include_directories(cardgame_core PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

if (DEVILS_ENGINE_BUILD_TESTS)
  add_executable(cardgame_headless_smoke headless_smoke.cpp)
  target_link_libraries(cardgame_headless_smoke PRIVATE cardgame::core devils_engine::options)
  set_target_properties(cardgame_headless_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/cardgame/bin")
  add_test(NAME cardgame_headless_smoke COMMAND cardgame_headless_smoke)
  set_tests_properties(cardgame_headless_smoke PROPERTIES LABELS devils_engine)
endif()
