cmake_minimum_required(VERSION 3.30)

project(GN07_location_brief VERSION 0.1.0 LANGUAGES CXX)

# Headless: задание сводится к графу, граф воплощается в план участка; вывод — отчёт, карта
# решётки и, по ключу, вид плана в SVG.
add_executable(GN07_location_brief
  src/main.cpp
  src/frame.cpp
  src/location_model.cpp
  src/mine_family.cpp
  src/node_scope.cpp
  src/resources.cpp
  src/solver.cpp
  src/tavl_reader.cpp)

target_compile_definitions(GN07_location_brief PRIVATE
  GN07_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources")

target_link_libraries(GN07_location_brief PRIVATE
  devils_engine::utils
  devils_engine::options
  devils_script
  tavl)

set_target_properties(GN07_location_brief PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/GN07_location_brief/bin")

if (DEVILS_ENGINE_BUILD_TESTS)
  set(GN07_BRIEFS ${CMAKE_CURRENT_SOURCE_DIR}/resources/gn07/briefs)
  foreach(BRIEF first_mine big_mine plain_mine)
    add_test(NAME GN07_${BRIEF}_verify
      COMMAND GN07_location_brief --verify --brief=${GN07_BRIEFS}/${BRIEF}.tavl)
    set_tests_properties(GN07_${BRIEF}_verify PROPERTIES LABELS devils_engine)
  endforeach()

  # Невыполнимое задание обязано отказать С ПРИЧИНОЙ, а не выдать частичный граф.
  add_test(NAME GN07_refuse_budget
    COMMAND GN07_location_brief --brief=${GN07_BRIEFS}/refuse_budget.tavl)
  set_tests_properties(GN07_refuse_budget PROPERTIES LABELS devils_engine
    PASS_REGULAR_EXPRESSION "refused: no attempt of 64 satisfied the brief; most frequent cause \\(64/64\\): main line blocked")

  add_test(NAME GN07_refuse_capability
    COMMAND GN07_location_brief --brief=${GN07_BRIEFS}/refuse_capability.tavl)
  set_tests_properties(GN07_refuse_capability PROPERTIES LABELS devils_engine
    PASS_REGULAR_EXPRESSION "refused: link to 'tunnel_3' is secret: archetype 'mine' has no secret port")

  add_test(NAME GN07_refuse_no_entrance
    COMMAND GN07_location_brief --brief=${GN07_BRIEFS}/refuse_no_entrance.tavl)
  set_tests_properties(GN07_refuse_no_entrance PROPERTIES LABELS devils_engine
    PASS_REGULAR_EXPRESSION "refused: location 'mine_4' has no ordinary link")

  add_test(NAME GN07_refuse_allowance
    COMMAND GN07_location_brief --brief=${GN07_BRIEFS}/refuse_allowance.tavl)
  set_tests_properties(GN07_refuse_allowance PROPERTIES LABELS devils_engine
    PASS_REGULAR_EXPRESSION "refused: placement 'quest_n_item' needs at least 12 content on its way, the whole location allows at most 11")
endif()
