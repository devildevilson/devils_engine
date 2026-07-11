cmake_minimum_required(VERSION 3.30)

project(
  devils_simul
  VERSION 0.1.1
  LANGUAGES CXX
)

add_library(devils_simul
  src/simul/interface.cpp
  src/simul/lua_script_resource.cpp
  src/simul/startup_resources.cpp
  include/devils_engine/simul/interface.h
  include/devils_engine/simul/boot_config.h
  include/devils_engine/simul/app_runtime.h
  include/devils_engine/simul/lua_script_resource.h
  include/devils_engine/simul/lua_resource_bindings.h
  include/devils_engine/simul/lifecycle.h
  include/devils_engine/simul/loading_runtime.h
  include/devils_engine/simul/resource_access_scope.h
  include/devils_engine/simul/instance_layout.h
  include/devils_engine/simul/draw_intent.h
  include/devils_engine/simul/interpolation.h
  include/devils_engine/simul/main_runtime.h
  include/devils_engine/simul/messages.h
  include/devils_engine/simul/render_config.h
  include/devils_engine/simul/render_runtime.h
  include/devils_engine/simul/standard_broker.h
  include/devils_engine/simul/standard_assets_system.h
  include/devils_engine/simul/standard_sound_system.h
  include/devils_engine/simul/startup_resources.h
  include/devils_engine/simul/systems.h
  include/devils_engine/simul/window_runtime.h
  include/devils_engine/simul/write_buffer_channel.h
)

target_include_directories(devils_simul
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
  PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/simul>"
)

target_link_libraries(devils_simul
  PUBLIC devils_engine::options devils_engine::utils devils_engine::demiurg
  PRIVATE tavl
)

add_library(devils_engine::simul ALIAS devils_simul)
