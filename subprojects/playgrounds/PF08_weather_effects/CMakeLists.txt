cmake_minimum_required(VERSION 3.25)

# PF08 начинает с закрытого PF07 baseline. Небесная механика остаётся headless и не зависит от
# Vulkan; погодные графические срезы живут отдельно от календарных режимов того же executable.
add_executable(PF08_weather_effects
  src/main.cpp
  src/celestial.cpp src/celestial.h
  src/survey.cpp src/survey.h
  src/sky_frame.cpp src/sky_frame.h
  src/fixture.cpp src/fixture.h
  src/shadows.cpp src/shadows.h
  src/terrain.cpp src/terrain.h
  src/foliage.cpp src/foliage.h
  src/weather.cpp src/weather.h
  src/surface_weather.cpp src/surface_weather.h
  src/sky_view.cpp src/sky_view.h
)
target_link_libraries(PF08_weather_effects PRIVATE
  devils_engine::playground_common
  devils_engine::painter
  devils_engine::input
  devils_engine::utils
  devils_engine::options
  devils_engine::visage
  glm::glm
  tavl
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp
)
target_compile_definitions(PF08_weather_effects PRIVATE
  PF08_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources"
  PLAYGROUND_COMMON_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../common/resources"
)

set_target_properties(PF08_weather_effects PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/PF08_weather_effects/bin"
)

add_custom_command(TARGET PF08_weather_effects POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
    "$<TARGET_FILE_DIR:PF08_weather_effects>/../cache"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    "$<TARGET_FILE_DIR:PF08_weather_effects>/resources"
  VERBATIM
)
