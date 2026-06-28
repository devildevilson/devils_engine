cmake_minimum_required(VERSION 3.30)

include(FetchContent)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_GENERATOR MATCHES "Visual Studio|Xcode|Ninja Multi-Config")
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_CONFIGURATION_TYPES
  Debug
  Release
  MinSizeRel
  RelWithDebInfo
  CACHE STRING "Build configurations" FORCE
)

project(
  devils_engine
  VERSION 0.1.1
  LANGUAGES CXX C
)

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

set(DEVILS_ENGINE_NAME devils_engine)

#set(DEVILS_ENGINE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${DEVILS_ENGINE_NAME}")
#set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
#set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${DEVILS_ENGINE_OUTPUT_DIRECTORY}/bin")
#set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${DEVILS_ENGINE_OUTPUT_DIRECTORY}/bin")

set(PROJECT_EXTERNAL_DEPENDENCY_DIR ${CMAKE_CURRENT_LIST_DIR}/external)

# devils_engine::options (compile/link flags + header-only include dirs) lives
# in libs/options and is added below, after FetchContent_MakeAvailable().

option(DEVILS_ENGINE_BUILD_TESTS "Build devils_engine tests" ON)
option(DEVILS_ENGINE_BUILD_VENDOR_TESTS "Build tests provided by third-party dependencies" OFF)

# 3 dependencies for user to deal with
if (WIN32) # freetype has proper cmake support
  set(FREETYPE_LIBRARY ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/freetype/freetype.lib)
  set(FREETYPE_LIBRARY_DIR ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/freetype)
  set(FREETYPE_INCLUDE_DIRS ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/freetype/include)
endif()

if (WIN32)
  set(GLFW3_LIBRARY_NAME glfw3dll)
else()
  #find_package(glfw3 REQUIRED)
  #set(GLFW3_LIBRARY_NAME glfw3)
endif()

set(BUILD_TESTING ${DEVILS_ENGINE_BUILD_VENDOR_TESTS} CACHE BOOL "Build tests for third-party dependencies" FORCE)

set(ZLIB_BUILD_EXAMPLES OFF)
set(ZLIB_BUILD_TESTING OFF)
set(ZLIB_ENABLE_TESTS ${DEVILS_ENGINE_BUILD_VENDOR_TESTS} CACHE BOOL "" FORCE)
set(ZLIBNG_ENABLE_TESTS ${DEVILS_ENGINE_BUILD_VENDOR_TESTS} CACHE BOOL "" FORCE)
set(WITH_GTEST ${DEVILS_ENGINE_BUILD_VENDOR_TESTS} CACHE BOOL "" FORCE)
set(PNG_STATIC OFF)
set(PNG_TOOLS OFF)
set(PNG_EXECUTABLES OFF)
set(PNG_TESTS ${DEVILS_ENGINE_BUILD_VENDOR_TESTS} CACHE BOOL "" FORCE)

set(MSDFGEN_DISABLE_SVG ON)
set(MSDFGEN_USE_SKIA OFF)
set(MSDFGEN_USE_VCPKG OFF)
set(MSDFGEN_BUILD_STANDALONE OFF)
#set(MSDFGEN_DISABLE_PNG ON)

set(MSDF_ATLAS_MSDFGEN_EXTERNAL ON CACHE BOOL "Use the project-managed msdfgen target" FORCE)
set(MSDF_ATLAS_BUILD_STANDALONE OFF CACHE BOOL "Do not build the msdf-atlas-gen standalone executable" FORCE)
set(MSDF_ATLAS_USE_VCPKG OFF CACHE BOOL "Do not let msdf-atlas-gen configure vcpkg" FORCE)
set(MSDF_ATLAS_USE_SKIA OFF CACHE BOOL "Do not build msdf-atlas-gen with Skia" FORCE)
set(MSDF_ATLAS_NO_ARTERY_FONT ON CACHE BOOL "Do not require the artery-font-format submodule" FORCE)

set(SPDLOG_BUILD_EXAMPLE OFF)
set(SPDLOG_BUILD_TESTS OFF)
set(SPDLOG_USE_STD_FORMAT ON)

set(ZLIB_COMPAT ON)

set(DYNAMICBITSET_USE_STD_BITOPS ON)
set(DYNAMICBITSET_BUILD_EXAMPLE OFF)
set(DYNAMICBITSET_BUILD_DOCS OFF)
set(DYNAMICBITSET_FORMAT_TARGET OFF)

set(CPUINFO_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(CPUINFO_BUILD_UNIT_TESTS ${DEVILS_ENGINE_BUILD_VENDOR_TESTS} CACHE BOOL "" FORCE)
set(CPUINFO_BUILD_MOCK_TESTS ${DEVILS_ENGINE_BUILD_VENDOR_TESTS} CACHE BOOL "" FORCE)
set(CPUINFO_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(CPUINFO_BUILD_PKG_CONFIG OFF CACHE BOOL "" FORCE)

set(VULKAN_HEADERS_ENABLE_INSTALL OFF)
set(VMA_BUILD_SAMPLES OFF)

set(GLM_BUILD_LIBRARY OFF)
set(GLM_ENABLE_CXX_20 ON)
set(GLM_ENABLE_SIMD_AVX ON)

set(MINIAUDIO_USE_STDINT ON)
set(MINIAUDIO_INSTALL OFF)
set(MINIAUDIO_NO_WAV ON)
set(MINIAUDIO_NO_FLAC ON)
set(MINIAUDIO_NO_MP3 ON)
set(MINIAUDIO_NO_LIBVORBIS ON)
set(MINIAUDIO_NO_LIBOPUS ON)

set(TAVL_BUILD_TESTS ${DEVILS_ENGINE_BUILD_VENDOR_TESTS} CACHE BOOL "" FORCE)
set(DS_BUILD_TESTS ${DEVILS_ENGINE_BUILD_VENDOR_TESTS} CACHE BOOL "" FORCE)
set(DS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  glaze
  GIT_REPOSITORY https://github.com/stephenberry/glaze.git
  GIT_TAG        v5.2.0 # version 5.1.2
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
  FIND_PACKAGE_ARGS
)

#FetchContent_Declare(
#  alpaca
#  GIT_REPOSITORY https://github.com/p-ranav/alpaca.git
#  GIT_TAG        83a592f0c3807500f1aaf3b07fd48105a01e2780 # Oct 1, 2024
#  GIT_SHALLOW    TRUE
#  GIT_PROGRESS   TRUE
#  OVERRIDE_FIND_PACKAGE 
#)

FetchContent_Declare(
  zpp_bits
  GIT_REPOSITORY https://github.com/eyalz800/zpp_bits.git
  GIT_TAG        v4.5
  GIT_PROGRESS   TRUE
  SOURCE_SUBDIR somePathThatNeverExistsAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAa
)

FetchContent_Declare(
  gtl
  GIT_REPOSITORY https://github.com/greg7mdp/gtl.git
  #GIT_TAG        v1.2.0
  GIT_TAG        e302ad9e3b6810bcc1ba2419eb5aaf5f32056823
  GIT_PROGRESS   TRUE
  OVERRIDE_FIND_PACKAGE 
)

FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.x
  GIT_PROGRESS   TRUE
  FIND_PACKAGE_ARGS
)

FetchContent_Declare(
  ZLIB
  GIT_REPOSITORY https://github.com/zlib-ng/zlib-ng.git
  GIT_TAG        2.2.4
  GIT_PROGRESS   TRUE
  OVERRIDE_FIND_PACKAGE 
)

FetchContent_Declare(
  PNG
  GIT_REPOSITORY https://github.com/pnggroup/libpng.git
  GIT_TAG        v1.6.48
  GIT_PROGRESS   TRUE
  OVERRIDE_FIND_PACKAGE 
)

FetchContent_Declare(
  minizip-ng
  GIT_REPOSITORY https://github.com/zlib-ng/minizip-ng.git
  GIT_TAG        4.0.10
  GIT_PROGRESS   TRUE
  FIND_PACKAGE_ARGS
)

FetchContent_Declare(
  atomic_queue
  GIT_REPOSITORY https://github.com/max0x7ba/atomic_queue.git
  GIT_TAG        v1.6.9
  GIT_PROGRESS   TRUE
)

FetchContent_Declare(
  nuklear
  GIT_REPOSITORY https://github.com/Immediate-Mode-UI/Nuklear.git
  GIT_TAG        4.12.7
  GIT_PROGRESS   TRUE
  SOURCE_SUBDIR somePathThatNeverExistsAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAa
)

FetchContent_Declare(
  msdfgen
  GIT_REPOSITORY https://github.com/Chlumsky/msdfgen.git
  GIT_TAG        v1.12
  GIT_PROGRESS   TRUE
  OVERRIDE_FIND_PACKAGE
)

FetchContent_Declare(
  msdf-atlas-gen
  GIT_REPOSITORY https://github.com/Chlumsky/msdf-atlas-gen.git
  GIT_TAG        v1.3
  GIT_SUBMODULES ""
  GIT_PROGRESS   TRUE
  OVERRIDE_FIND_PACKAGE
)

FetchContent_Declare(
  dr_libs
  GIT_REPOSITORY https://github.com/mackron/dr_libs.git
  GIT_TAG        9cb7092ac8c75a82b5c6ea72652ca8d0091d7ffa
  GIT_PROGRESS   TRUE
  SOURCE_SUBDIR somePathThatNeverExistsAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAa
)

FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        f0569113c93ad095470c54bf34a17b36646bbbb5
  GIT_PROGRESS   TRUE
  SOURCE_SUBDIR somePathThatNeverExistsAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAa
)

FetchContent_Declare(
  reflect
  GIT_REPOSITORY https://github.com/qlibs/reflect.git
  GIT_TAG        v1.2.6
  GIT_PROGRESS   TRUE
  SOURCE_SUBDIR somePathThatNeverExistsAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAa
)

FetchContent_Declare(
  glfw3
  URL  https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.bin.WIN64.zip
  FIND_PACKAGE_ARGS
)

FetchContent_Declare(
  sol2 
  GIT_REPOSITORY https://github.com/ThePhD/sol2.git
  GIT_TAG        v3.5.0
  GIT_PROGRESS   TRUE
  FIND_PACKAGE_ARGS
)

FetchContent_Declare(
  Vulkan-Headers
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG        v1.4.314
  GIT_PROGRESS   TRUE
  OVERRIDE_FIND_PACKAGE
)

FetchContent_Declare(
  VulkanMemoryAllocator
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG        v3.2.1
  GIT_PROGRESS   TRUE
  OVERRIDE_FIND_PACKAGE
)

FetchContent_Declare(
  VulkanMemoryAllocator-Hpp
  GIT_REPOSITORY https://github.com/YaaZ/VulkanMemoryAllocator-Hpp.git
  GIT_TAG        v3.2.1
  GIT_SUBMODULES ""
  SOURCE_SUBDIR  include
  GIT_PROGRESS   TRUE
)

FetchContent_Declare(
  dynamic_bitset
  GIT_REPOSITORY https://github.com/pinam45/dynamic_bitset.git
  GIT_TAG        v1.3.2
  GIT_PROGRESS   TRUE
  FIND_PACKAGE_ARGS
)

FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG        1.0.1
  GIT_PROGRESS   TRUE
  FIND_PACKAGE_ARGS
)

FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.5.2
  GIT_PROGRESS   TRUE
  FIND_PACKAGE_ARGS
)

#FetchContent_Declare(
#  RE-flex
#  GIT_REPOSITORY https://github.com/Genivia/RE-flex.git
#  GIT_TAG        v6.0.0
#  GIT_PROGRESS   TRUE
#  FIND_PACKAGE_ARGS
#)

FetchContent_Declare(
  cpuinfo
  GIT_REPOSITORY https://github.com/pytorch/cpuinfo.git
  GIT_TAG        c4b4f4bf08c0cf486fc3111d0244ebf2a48ad01b
  GIT_PROGRESS   TRUE
  FIND_PACKAGE_ARGS
)

FetchContent_Declare(
  miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG        0.11.25
  GIT_PROGRESS   TRUE
  FIND_PACKAGE_ARGS
)

FetchContent_Declare(
  rapidhash
  GIT_REPOSITORY https://github.com/Nicoshev/rapidhash.git
  GIT_TAG        rapidhash_v3
  GIT_PROGRESS   TRUE
  FIND_PACKAGE_ARGS
)

FetchContent_Declare(
  tavl
  GIT_REPOSITORY https://github.com/devildevilson/tavl.git
  GIT_TAG        v1.0.2
  OVERRIDE_FIND_PACKAGE
)

FetchContent_Declare(
  devils_script
  GIT_REPOSITORY https://github.com/devildevilson/devils_script.git
  GIT_TAG        v1.2.0
  OVERRIDE_FIND_PACKAGE
)

FetchContent_MakeAvailable(ZLIB)

# workarounds for zlib-ng
add_library(ZLIB::ZLIB ALIAS zlib)
set(ZLIB_INCLUDE_DIR "${FETCHCONTENT_BASE_DIR}/zlib-build/")
set(ZLIB_INCLUDE_DIRS "${FETCHCONTENT_BASE_DIR}/zlib-build/")

FetchContent_MakeAvailable(glaze zpp_bits gtl spdlog minizip-ng PNG atomic_queue nuklear msdfgen msdf-atlas-gen dr_libs stb glfw3 sol2 Vulkan-Headers VulkanMemoryAllocator VulkanMemoryAllocator-Hpp dynamic_bitset glm doctest cpuinfo miniaudio rapidhash tavl devils_script reflect) # RE-flex

add_library(VulkanMemoryAllocator-Hpp INTERFACE)
add_library(VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp ALIAS VulkanMemoryAllocator-Hpp)
target_include_directories(VulkanMemoryAllocator-Hpp INTERFACE "${vulkanmemoryallocator-hpp_SOURCE_DIR}/include")
target_link_libraries(VulkanMemoryAllocator-Hpp INTERFACE Vulkan::Headers GPUOpen::VulkanMemoryAllocator)

#target_compile_options(spdlog 
#  PRIVATE /fsanitize=address
#)

find_package(Threads REQUIRED)

# workarounds for other libs
add_library(PNG::PNG ALIAS png_shared)

if (DEVILS_ENGINE_BUILD_TESTS OR DEVILS_ENGINE_BUILD_VENDOR_TESTS)
  enable_testing()
endif()

if(MSVC)
  set(DYN_LIBRARIES_LIST
    libs/lua54/lua54.dll
    libs/openal/OpenAL32.dll
    libs/openal/soft_oal.dll
    libs/glfw34/glfw3.dll
    libs/ktx/ktx.dll
    libs/vulkan/shaderc_shared.dll
    libs/freetype/freetype.dll
    libs/libpng16/libpng16.dll
    libs/brotli/brotlicommon.dll
    libs/brotli/brotlidec.dll
    libs/brotli/brotlienc.dll
  )
else()
  set(DYN_LIBRARIES_LIST
    
  )
endif()

#add_compile_definitions("DEVILS_ENGINE_PROJECT_NAME=\"${PROJECT_NAME}\"")
#add_compile_definitions("DEVILS_ENGINE_ENGINE_NAME=\"${DEVILS_ENGINE_NAME}\"")

# Link each target with other targets or add options, etc.
add_subdirectory(libs/options/)
add_subdirectory(libs/utils/)
add_subdirectory(libs/mood/)
add_subdirectory(libs/simul/)
add_subdirectory(libs/aesthetics/)
add_subdirectory(libs/sound/)
add_subdirectory(libs/demiurg/)
add_subdirectory(libs/input/)
add_subdirectory(libs/painter/)
add_subdirectory(libs/visage/)
add_subdirectory(libs/bindings/)
add_subdirectory(libs/acumen/)
add_subdirectory(libs/catalogue/)

add_library(devils_engine INTERFACE)
target_link_libraries(devils_engine INTERFACE devils_engine::catalogue devils_engine::acumen devils_engine::demiurg devils_engine::bindings devils_engine::input devils_engine::painter devils_engine::sound devils_engine::utils devils_engine::mood devils_engine::simul devils_engine::aesthetics devils_engine::visage devils_engine::options)
add_library(devils_engine::devils_plane ALIAS devils_engine)

if (DEVILS_ENGINE_BUILD_TESTS)
  add_subdirectory(tests/)
endif()
add_subdirectory(tests/tile_frontier/)

if (DEVILS_ENGINE_BUILD_TESTS)
  add_custom_target(devils_engine_test
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure -L devils_engine
    DEPENDS utils_general_test utils_contract_test thread_general_test aesthetics_world_test sound_system_test
    USES_TERMINAL
    COMMENT "Running devils_engine tests"
  )
endif()

if (DEVILS_ENGINE_BUILD_VENDOR_TESTS)
  add_custom_target(devils_engine_vendor_test
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure -LE devils_engine
    USES_TERMINAL
    COMMENT "Running third-party dependency tests"
  )
else()
  add_custom_target(devils_engine_vendor_test
    COMMAND ${CMAKE_COMMAND} -E echo "Third-party dependency tests are disabled. Reconfigure with -DDEVILS_ENGINE_BUILD_VENDOR_TESTS=ON to build and run them."
    USES_TERMINAL
  )
endif()

#foreach (library_path IN LISTS DYN_LIBRARIES_LIST)
#  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD 
#    COMMAND "${CMAKE_COMMAND}" -E copy 
#        "${CMAKE_SOURCE_DIR}/${library_path}"
#        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" 
#    COMMENT "Copy libs")
#endforeach()

if (WIN32)
  set(SHADERC_LIBRARY_NAME shaderc_shared)
  set(VULKAN_LIBRARY_NAME vulkan-1)
else()
  find_package(Vulkan REQUIRED)
  find_library(SHADERC_LIB NAMES shaderc_combined shaderc)
  set(SHADERC_LIBRARY_NAME ${SHADERC_LIB})
  set(VULKAN_LIBRARY_NAME Vulkan::Vulkan)
endif()

set(FAST_TEST_TARGET fast_test)
set(FAST_TEST_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${FAST_TEST_TARGET}")

add_executable(${FAST_TEST_TARGET} main.cpp)
target_link_libraries(${FAST_TEST_TARGET} devils_engine::devils_plane ${SHADERC_LIBRARY_NAME} Vulkan::Headers GPUOpen::VulkanMemoryAllocator VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp)
set_target_properties(${FAST_TEST_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${FAST_TEST_OUTPUT_DIRECTORY}/bin")

set(GRAPHICS_TEST_CONFIG_LOCATION ${CMAKE_CURRENT_LIST_DIR}/tests/)

add_custom_command(TARGET ${FAST_TEST_TARGET} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory 
    ${GRAPHICS_TEST_CONFIG_LOCATION}/test_render_config 
    ${FAST_TEST_OUTPUT_DIRECTORY}/tests/render_config
)

add_custom_command(TARGET ${FAST_TEST_TARGET} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory 
    ${GRAPHICS_TEST_CONFIG_LOCATION}/shaders 
    ${FAST_TEST_OUTPUT_DIRECTORY}/tests/shaders
)

if (WIN32)
  add_custom_command(TARGET ${FAST_TEST_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:${FAST_TEST_TARGET}>
      $<TARGET_FILE_DIR:${FAST_TEST_TARGET}>
    COMMAND_EXPAND_LISTS
  )
endif()
