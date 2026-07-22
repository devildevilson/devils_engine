cmake_minimum_required(VERSION 3.30)

project(cardgame VERSION 0.0.1 LANGUAGES CXX)

add_library(cardgame_core STATIC
  include/cardgame/effect_program.h
  include/cardgame/follow_up.h
  include/cardgame/combat.h
  include/cardgame/combat_script.h
  src/effect_program.cpp
  src/follow_up.cpp
  src/combat.cpp
  src/combat_script.cpp
)
add_library(cardgame::core ALIAS cardgame_core)
target_link_libraries(cardgame_core PUBLIC
  devils_engine::resolve
  devils_engine::simul
  devils_engine::act
  devils_engine::options
)
target_include_directories(cardgame_core PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

if (DEVILS_ENGINE_BUILD_TESTS)
  add_executable(cardgame_effect_program_test effect_program_test.cpp)
  target_link_libraries(cardgame_effect_program_test PRIVATE cardgame::core devils_engine::options)
  set_target_properties(cardgame_effect_program_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/cardgame/bin")
  add_test(NAME cardgame_effect_program_test COMMAND cardgame_effect_program_test)
  set_tests_properties(cardgame_effect_program_test PROPERTIES LABELS devils_engine)

  add_executable(cardgame_follow_up_test follow_up_test.cpp)
  target_link_libraries(cardgame_follow_up_test PRIVATE cardgame::core devils_engine::options)
  set_target_properties(cardgame_follow_up_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/cardgame/bin")
  add_test(NAME cardgame_follow_up_test COMMAND cardgame_follow_up_test)
  set_tests_properties(cardgame_follow_up_test PROPERTIES LABELS devils_engine)

  add_executable(cardgame_action_pipeline_test action_pipeline_test.cpp)
  target_link_libraries(cardgame_action_pipeline_test PRIVATE cardgame::core devils_engine::options)
  set_target_properties(cardgame_action_pipeline_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/cardgame/bin")
  add_test(NAME cardgame_action_pipeline_test COMMAND cardgame_action_pipeline_test)
  set_tests_properties(cardgame_action_pipeline_test PROPERTIES LABELS devils_engine)

  add_executable(cardgame_typed_effect_test typed_effect_test.cpp)
  target_link_libraries(cardgame_typed_effect_test PRIVATE cardgame::core devils_engine::options)
  target_compile_definitions(cardgame_typed_effect_test PRIVATE
    CARDGAME_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources/")
  set_target_properties(cardgame_typed_effect_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/cardgame/bin")
  add_test(NAME cardgame_typed_effect_test COMMAND cardgame_typed_effect_test)
  set_tests_properties(cardgame_typed_effect_test PROPERTIES LABELS devils_engine)

  add_executable(cardgame_combat_script_test combat_script_test.cpp)
  target_link_libraries(cardgame_combat_script_test PRIVATE cardgame::core devils_engine::options)
  target_compile_definitions(cardgame_combat_script_test PRIVATE
    CARDGAME_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources/")
  set_target_properties(cardgame_combat_script_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/cardgame/bin")
  add_test(NAME cardgame_combat_script_test COMMAND cardgame_combat_script_test)
  set_tests_properties(cardgame_combat_script_test PROPERTIES LABELS devils_engine)

  add_executable(cardgame_headless_smoke headless_smoke.cpp)
  target_link_libraries(cardgame_headless_smoke PRIVATE cardgame::core devils_engine::options)
  target_compile_definitions(cardgame_headless_smoke PRIVATE
    CARDGAME_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources/")
  set_target_properties(cardgame_headless_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/cardgame/bin")
  add_test(NAME cardgame_headless_smoke COMMAND cardgame_headless_smoke)
  set_tests_properties(cardgame_headless_smoke PROPERTIES LABELS devils_engine)
endif()
