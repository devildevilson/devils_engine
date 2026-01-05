cmake_minimum_required(VERSION 3.30)

include(FetchContent)

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

set(DEVILS_ENGINE_NAME "devils_engine")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")

set(PROJECT_EXTERNAL_DEPENDENCY_DIR ${CMAKE_CURRENT_LIST_DIR}/external)

add_library(devils_engine_options INTERFACE)
target_compile_features(devils_engine_options INTERFACE cxx_std_23)
#target_compile_features(devils_engine_options INTERFACE c_std_11)
target_compile_options(devils_engine_options INTERFACE
  $<$<CONFIG:Debug>:$<$<CXX_COMPILER_ID:MSVC>:/arch:AVX /MDd /ZI /Wall /wd5045 /wd4820 /GR- /permissive- /Zc:preprocessor>>
  $<$<CONFIG:Release>:$<$<CXX_COMPILER_ID:MSVC>:/arch:AVX /MD /Wall /wd5045 /wd4820 /O2 /GR- /permissive- /Zc:preprocessor>>
  $<$<CONFIG:MinSizeRel>:$<$<CXX_COMPILER_ID:MSVC>:/arch:AVX /MD /Wall /wd5045 /wd4820 /O2 /GR- /permissive- /Zc:preprocessor>>
  $<$<CONFIG:RelWithDebInfo>:$<$<CXX_COMPILER_ID:MSVC>:/arch:AVX /MDd /ZI /Wall /wd5045 /wd4820 /O2 /GR- /permissive- /Zc:preprocessor>>

  $<$<CONFIG:Debug>:$<$<CXX_COMPILER_ID:GNU,Clang>:-mavx -g -Wall -Wextra -fno-rtti -fno-inline>>
  $<$<CONFIG:Release>:$<$<CXX_COMPILER_ID:GNU,Clang>:-mavx -O3 -Wall -Wextra -fno-rtti -pipe -flto>>
  $<$<CONFIG:MinSizeRel>:$<$<CXX_COMPILER_ID:GNU,Clang>:-mavx -O3 -Wall -Wextra -fno-rtti -pipe -flto -s>>
  $<$<CONFIG:RelWithDebInfo>:$<$<CXX_COMPILER_ID:GNU,Clang>:-mavx -O3 -g -Wall -Wextra -fno-rtti -pipe -flto>>
)
target_compile_definitions(devils_engine_options INTERFACE
  $<$<CONFIG:Debug>:>
  $<$<CONFIG:Release>:NDEBUG _NDEBUG>
  $<$<CONFIG:MinSizeRel>:NDEBUG _NDEBUG>
  $<$<CONFIG:RelWithDebInfo>:>
)
target_compile_definitions(devils_engine_options INTERFACE
  DEVILS_ENGINE_ENGINE_NAME="${DEVILS_ENGINE_NAME}"
)

add_library(devils_engine::options ALIAS devils_engine_options)

# 3 dependencies for user to deal with
if (WIN32) # freetype has proper cmake support
  set(FREETYPE_LIBRARY ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/freetype/freetype.lib)
  set(FREETYPE_LIBRARY_DIR ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/freetype)
  set(FREETYPE_INCLUDE_DIRS ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/freetype/include)
endif()

include_directories(${FETCHCONTENT_BASE_DIR}/dr_libs-src/)
include_directories(${FETCHCONTENT_BASE_DIR}/stb-src/)
include_directories(${FETCHCONTENT_BASE_DIR}/zpp_bits-src/)

if (WIN32)
  set(GLFW3_LIBRARY_NAME glfw3dll)
else()
  #find_package(glfw3 REQUIRED)
  set(GLFW3_LIBRARY_NAME glfw3)
endif()

set(ZLIB_BUILD_EXAMPLES OFF)
set(ZLIB_BUILD_TESTING OFF)
set(PNG_STATIC OFF)
set(PNG_TOOLS OFF)
set(PNG_EXECUTABLES OFF)

set(MSDFGEN_DISABLE_SVG ON)
set(MSDFGEN_USE_SKIA OFF)
set(MSDFGEN_USE_VCPKG OFF)
set(MSDFGEN_BUILD_STANDALONE OFF)
#set(MSDFGEN_DISABLE_PNG ON)

set(MSDF_ATLAS_USE_VCPKG OFF)
set(MSDF_ATLAS_USE_SKIA OFF)
set(MSDF_ATLAS_BUILD_STANDALONE OFF)
set(MSDF_ATLAS_MSDFGEN_EXTERNAL ON)

set(SPDLOG_USE_STD_FORMAT ON)

set(ZLIB_COMPAT ON)

set(DYNAMICBITSET_USE_STD_BITOPS ON)
set(DYNAMICBITSET_BUILD_EXAMPLE OFF)
set(DYNAMICBITSET_BUILD_DOCS OFF)
set(DYNAMICBITSET_FORMAT_TARGET OFF)

set(VULKAN_HEADERS_ENABLE_INSTALL OFF)
set(VMA_BUILD_SAMPLES OFF)

set(GLM_BUILD_LIBRARY OFF)
set(GLM_ENABLE_CXX_20 ON)
set(GLM_ENABLE_SIMD_AVX ON)

set(CATCH_INSTALL_DOCS OFF)
set(CATCH_INSTALL_EXTRAS OFF)

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
  GIT_TAG        v1.2.0
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
  GIT_PROGRESS   TRUE
  OVERRIDE_FIND_PACKAGE
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
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.8.1
  GIT_PROGRESS   TRUE
  FIND_PACKAGE_ARGS
)

FetchContent_Declare(
  RE-flex
  GIT_REPOSITORY https://github.com/Genivia/RE-flex.git
  GIT_TAG        v6.0.0
  GIT_PROGRESS   TRUE
  FIND_PACKAGE_ARGS
)

FetchContent_MakeAvailable(ZLIB)

# workarounds for zlib-ng
add_library(ZLIB::ZLIB ALIAS zlib)
set(ZLIB_INCLUDE_DIR "${FETCHCONTENT_BASE_DIR}/zlib-build/")
set(ZLIB_INCLUDE_DIRS "${FETCHCONTENT_BASE_DIR}/zlib-build/")

FetchContent_MakeAvailable(glaze zpp_bits gtl spdlog minizip-ng PNG atomic_queue nuklear msdfgen msdf-atlas-gen dr_libs stb glfw3 sol2 Vulkan-Headers VulkanMemoryAllocator VulkanMemoryAllocator-Hpp dynamic_bitset glm Catch2 RE-flex)

find_package(Threads REQUIRED)

# workarounds for other libs
add_library(PNG::PNG ALIAS png_shared)

# MSVC workaround
if ("cxx_std_17" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
  target_compile_features(Catch2 PRIVATE cxx_std_17)
  target_compile_features(Catch2WithMain PRIVATE cxx_std_17)
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

add_compile_definitions("DEVILS_ENGINE_PROJECT_NAME=\"${PROJECT_NAME}\"")
add_compile_definitions("DEVILS_ENGINE_ENGINE_NAME=\"${DEVILS_ENGINE_NAME}\"")

# Link each target with other targets or add options, etc.
add_subdirectory(libs/utils/)
add_subdirectory(libs/sound/)
add_subdirectory(libs/demiurg/)
add_subdirectory(libs/thread/)
add_subdirectory(libs/input/)
add_subdirectory(libs/painter/)
add_subdirectory(libs/visage/)
add_subdirectory(libs/bindings/)
add_subdirectory(libs/acumen/)
add_subdirectory(libs/catalogue/)
add_subdirectory(tests/)

#foreach (library_path IN LISTS DYN_LIBRARIES_LIST)
#  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD 
#    COMMAND "${CMAKE_COMMAND}" -E copy 
#        "${CMAKE_SOURCE_DIR}/${library_path}"
#        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" 
#    COMMENT "Copy libs")
#endforeach()

# вообще по итогу наверное лучше будет оформить единую либу со всеми зависимостями

add_library(devils_engine INTERFACE)
target_link_libraries(devils_engine INTERFACE devils_engine::catalogue devils_engine::acumen devils_engine::demiurg devils_engine::bindings devils_engine::input devils_engine::painter devils_engine::sound devils_engine::thread devils_engine::utils devils_engine::visage devils_engine::options)
add_library(devils_engine::devils_plane ALIAS devils_engine)

if (WIN32)
  set(SHADERC_LIBRARY_NAME shaderc_shared)
  set(VULKAN_LIBRARY_NAME vulkan-1)
else()
  set(SHADERC_LIBRARY_NAME libshaderc)
  set(VULKAN_LIBRARY_NAME vulkan)
endif()

add_executable(fast_test main.cpp)
target_link_libraries(fast_test devils_engine::devils_plane ${SHADERC_LIBRARY_NAME} Vulkan::Headers GPUOpen::VulkanMemoryAllocator VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp)