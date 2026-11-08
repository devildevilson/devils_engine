cmake_minimum_required(VERSION 3.30)

project(GN03_streaming_volume VERSION 0.1.0 LANGUAGES CXX)

# Площадка headless по сути: всё, что она доказывает, проверяется числами (`--verify`). Окно живёт в
# том же исполняемом файле не ради экономии, а потому что и проверке, и окну нужен ОДИН И ТОТ ЖЕ
# генератор: разнести их значило бы дважды собирать пайплайн и однажды собрать его по-разному.
add_executable(GN03_streaming_volume
  src/main.cpp
  src/skeleton.cpp src/skeleton.h
)

target_compile_definitions(GN03_streaming_volume PRIVATE
  GN03_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources"
)

target_link_libraries(GN03_streaming_volume PRIVATE
  devils_engine::playground_chunk_viewer
  devils_engine::originator
  devils_engine::originator_config
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

set_target_properties(GN03_streaming_volume PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/GN03_streaming_volume/bin")

if (DEVILS_ENGINE_BUILD_TESTS)
  # Проверка идёт на РАЗРЕШЕНИИ ПО УМОЛЧАНИЮ: чанк — единица генерации, и мельчить его в проверке
  # значило бы проверять другую геометрию. Считается при этом не мир, а десятка чанков, поэтому
  # проверка укладывается в секунды.
  devils_add_test(NAME GN03_streaming_volume_verify COMMAND GN03_streaming_volume --verify)
  set_tests_properties(GN03_streaming_volume_verify PROPERTIES LABELS devils_engine)
endif()

target_link_libraries(GN03_streaming_volume PRIVATE devils_engine::playground_cli devils_engine::playground_generator)
