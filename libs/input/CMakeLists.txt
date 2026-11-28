cmake_minimum_required(VERSION 3.25)

include(FetchContent)

project(
  devils_input
  VERSION 0.1
  LANGUAGES CXX C
)

# GLFW: the distribution's package on Linux, vcpkg on Windows. 3.4 is the floor: core.cpp uses
# glfwInitVulkanLoader and glfwGetWindowTitle (Ubuntu 24.04 ships 3.3). Consumers link
# devils_engine::input and receive GLFW through its private static link dependency.
find_package(glfw3 3.4 REQUIRED GLOBAL)

set(SRC_LIST
  src/input/bindings.cpp
  src/input/core.cpp
  src/input/events.cpp
  src/input/key_names.cpp
)

set(HDR_LIST
  include/devils_engine/input/bindings.h
  include/devils_engine/input/core.h
  include/devils_engine/input/events.h
  include/devils_engine/input/key_names.h
)

add_library(devils_input ${SRC_LIST} ${HDR_LIST})
target_link_libraries(devils_input PUBLIC devils_engine::options devils_engine::utils PRIVATE glfw Vulkan::Headers)
# The engine renders with Vulkan only. Without GLFW_INCLUDE_NONE glfw3.h also pulls in GL/gl.h,
# which a machine without OpenGL development headers does not have.
target_compile_definitions(devils_input PRIVATE GLFW_INCLUDE_NONE)
target_include_directories(devils_input INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>")
target_include_directories(devils_input PRIVATE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/input>")

#if (MSVC)
#  target_include_directories(devils_input PRIVATE ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/vulkan/Include)
#endif()

add_library(devils_engine::input ALIAS devils_input)
