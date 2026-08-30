cmake_minimum_required(VERSION 3.25)

# Срез 1 PF09 намеренно не знает про Vulkan: иерархия территорий обязана существовать и проверяться
# раньше любого растра, иначе сверять запечённый тексель будет не с чем. Графические зависимости
# появятся вместе со срезом 2 (ID-клипмап).
add_executable(PF09_territory_zoning
  src/main.cpp
  src/territory.cpp src/territory.h
  src/clipmap.cpp src/clipmap.h
  src/locality.cpp src/locality.h
  src/zones.cpp src/zones.h
  src/world_build.cpp src/world_build.h
  src/navigate.cpp src/navigate.h
)
target_link_libraries(PF09_territory_zoning PRIVATE
  devils_engine::utils
  glm::glm
)
target_compile_definitions(PF09_territory_zoning PRIVATE
  PF09_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources"
)

set_target_properties(PF09_territory_zoning PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/PF09_territory_zoning/bin"
)
