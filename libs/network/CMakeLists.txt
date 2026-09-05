cmake_minimum_required(VERSION 3.30)

project(
  devils_network
  VERSION 0.1.0
  LANGUAGES CXX
)

add_library(devils_network INTERFACE)
target_include_directories(devils_network INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
)
target_link_libraries(devils_network INTERFACE devils_engine::options devils_engine::utils)

add_library(devils_engine::network ALIAS devils_network)

# The neutral templates do not acquire GNS headers or linkage. Consumers opt
# into this concrete backend independently.
option(DEVILS_ENGINE_BUILD_NETWORK_GNS "Build the optional GNS message adapter" ON)
if (DEVILS_ENGINE_BUILD_NETWORK_GNS)
  add_library(devils_network_gns src/network/gns_transport.cpp)
  target_include_directories(devils_network_gns PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>")
  target_link_libraries(devils_network_gns PUBLIC
    devils_engine::options devils_engine::utils GameNetworkingSockets::static)
  add_library(devils_engine::network_gns ALIAS devils_network_gns)
endif()
