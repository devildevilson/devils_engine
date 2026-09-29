cmake_minimum_required(VERSION 3.30)

project(GN06_dungeon_layout VERSION 0.1.0 LANGUAGES CXX)

# Контрактные проверки остаются headless. Интерактивный показ использует собственную тонкую
# витрину зон GN06 (основана на графе PF09); генератор и его данные от графики не зависят.
add_executable(GN06_dungeon_layout src/main.cpp src/motif_demo.cpp src/motif_viewer.cpp src/site_graph.cpp
  src/viewer_detail_page.cpp)

target_compile_definitions(GN06_dungeon_layout PRIVATE
  GN06_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources"
  PLAYGROUND_COMMON_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../common/resources")

target_link_libraries(GN06_dungeon_layout PRIVATE
  devils_engine::originator
  devils_engine::originator_config
  devils_engine::originator_lua
  devils_engine::utils
  devils_engine::options
  devils_engine::playground_common
  devils_engine::painter
  devils_engine::input
  devils_engine::acumen
  devils_engine::visage
  glm::glm
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp
  tavl)

set_target_properties(GN06_dungeon_layout PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/GN06_dungeon_layout/bin")

if (DEVILS_ENGINE_BUILD_TESTS)
  # Два генератора — две проверки: у норы и у здания разные обещания, и зелёный прогон одного ничего
  # не говорит про другого.
  add_test(NAME GN06_cave_verify
    COMMAND GN06_dungeon_layout --mode=cave --verify --size=64)
  set_tests_properties(GN06_cave_verify PROPERTIES LABELS devils_engine)

  add_test(NAME GN06_building_verify
    COMMAND GN06_dungeon_layout --mode=building --verify --size=64)
  set_tests_properties(GN06_building_verify PROPERTIES LABELS devils_engine)

  # ТРЕТЬЯ ПРОВЕРКА — УЧАСТОК НЕ ПО РАЗМЕРУ КАРТИНКИ. Сторона участка выбрана нарочно не круглой и
  # заведомо не равной ни одному разумному растру: пока план жил в клетках, такой прогон был
  # невозможен вовсе, и зелёный он ровно потому, что картинку теперь никто не читает.
  add_test(NAME GN06_building_plot_verify
    COMMAND GN06_dungeon_layout --mode=building --verify --size=157)
  set_tests_properties(GN06_building_plot_verify PROPERTIES LABELS devils_engine)

  add_test(NAME GN06_motifs_verify
    COMMAND GN06_dungeon_layout --mode=motifs --verify --scale=medium --surface=cut)
  set_tests_properties(GN06_motifs_verify PROPERTIES LABELS devils_engine)

  add_test(NAME GN06_motifs_small_verify
    COMMAND GN06_dungeon_layout --mode=motifs --verify --scale=small --entry-y=20)
  set_tests_properties(GN06_motifs_small_verify PROPERTIES LABELS devils_engine)

  add_test(NAME GN06_motifs_large_verify
    COMMAND GN06_dungeon_layout --mode=motifs --verify --size=301 --surface=cut)
  set_tests_properties(GN06_motifs_large_verify PROPERTIES LABELS devils_engine)

  foreach(SCALE small medium large)
    add_test(NAME GN06_site_${SCALE}_verify
      COMMAND GN06_dungeon_layout --mode=site --verify --scale=${SCALE})
    set_tests_properties(GN06_site_${SCALE}_verify PROPERTIES LABELS devils_engine)
  endforeach()

  add_test(NAME GN06_site_perception_visible
    COMMAND GN06_dungeon_layout --mode=site --scale=medium --seed=2)
  set_tests_properties(GN06_site_perception_visible PROPERTIES
    LABELS devils_engine PASS_REGULAR_EXPRESSION "perception_candidates=[1-9][0-9]* perceptions=[1-9]")

  add_test(NAME GN06_site_perception_hidden
    COMMAND GN06_dungeon_layout --mode=site --scale=medium --seed=6)
  set_tests_properties(GN06_site_perception_hidden PROPERTIES
    LABELS devils_engine PASS_REGULAR_EXPRESSION "perception_candidates=[1-9][0-9]* perceptions=0")

  add_test(NAME GN06_site_surface_exposure
    COMMAND GN06_dungeon_layout --mode=site --scale=small --seed=0 --surface=cut)
  set_tests_properties(GN06_site_surface_exposure PROPERTIES
    LABELS devils_engine PASS_REGULAR_EXPRESSION "exposures=[1-9][0-9]*")

  add_test(NAME GN06_site_reserved_entry_verify
    COMMAND GN06_dungeon_layout --mode=site --scale=small --seed=0 --entry-y=20 --surface=cut --verify)
  set_tests_properties(GN06_site_reserved_entry_verify PROPERTIES LABELS devils_engine)

  add_test(NAME GN06_site_path_ending_trim
    COMMAND GN06_dungeon_layout --mode=site --scale=large --seed=0)
  set_tests_properties(GN06_site_path_ending_trim PROPERTIES LABELS devils_engine
    PASS_REGULAR_EXPRESSION "end policy=vary outcome=trim removed=[1-9]")

  add_test(NAME GN06_site_path_ending_accent
    COMMAND GN06_dungeon_layout --mode=site --scale=large --seed=0)
  set_tests_properties(GN06_site_path_ending_accent PROPERTIES LABELS devils_engine
    PASS_REGULAR_EXPRESSION "end policy=vary outcome=accent removed=0 accent=service_alcove")
endif()
