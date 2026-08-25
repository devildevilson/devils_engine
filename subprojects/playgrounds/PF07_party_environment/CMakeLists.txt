cmake_minimum_required(VERSION 3.25)

# Срез 1 сознательно не тянет painter/input/visage: небесная механика закрывается численно и
# headless, а графические зависимости появляются в срезе 2 вместе с небом.
add_executable(PF07_party_environment src/main.cpp src/celestial.cpp src/celestial.h src/survey.cpp src/survey.h)
target_link_libraries(PF07_party_environment PRIVATE
  devils_engine::utils
  glm::glm
  tavl
)
target_compile_definitions(PF07_party_environment PRIVATE
  PF07_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources"
)

set_target_properties(PF07_party_environment PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/PF07_party_environment/bin"
)

add_custom_command(TARGET PF07_party_environment POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    "$<TARGET_FILE_DIR:PF07_party_environment>/resources"
  VERBATIM
)
