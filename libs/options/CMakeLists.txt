cmake_minimum_required(VERSION 3.30)

# --- link-time optimization ---------------------------------------------------
# Enabled for the optimized configurations and propagated to both compilation
# and linking of every consumer. Toggle with -DDEVILS_ENGINE_ENABLE_LTO=OFF.
option(DEVILS_ENGINE_ENABLE_LTO "Enable link-time optimization for optimized builds" ON)

# devils_engine build options as a single INTERFACE target. Everything that
# links devils_engine::options inherits the language standard, warning /
# optimization flags, LTO, common compile definitions and the header-only
# third-party include directories. This target is added after
# FetchContent_MakeAvailable() so the _deps source paths below already exist.

add_library(devils_engine_options INTERFACE)
add_library(devils_engine::options ALIAS devils_engine_options)

# Required C++ standard. On an INTERFACE target, INTERFACE *is* the public face:
# the requirement is a usage requirement that propagates to every consumer. The
# sub-libraries link options PRIVATE, but the public umbrella target
# devils_engine::devils_plane links it with INTERFACE visibility, so any external
# project that links the engine is forced to compile with at least C++23.
target_compile_features(devils_engine_options INTERFACE cxx_std_23)
#target_compile_features(devils_engine_options INTERFACE c_std_11)

# --- target architecture ------------------------------------------------------
# Factored out so an external project can substitute its own target arch before
# add_subdirectory(libs/options), e.g.
#   set(DEVILS_ENGINE_ARCH_FLAGS_GNU  -march=native)
#   set(DEVILS_ENGINE_ARCH_FLAGS_MSVC /arch:AVX2)
# or pass them on the command line with -D...=... . Use a CMake list (semicolon
# separated) when you need more than one flag.
if (NOT DEFINED DEVILS_ENGINE_ARCH_FLAGS_MSVC)
  set(DEVILS_ENGINE_ARCH_FLAGS_MSVC /arch:AVX CACHE STRING "MSVC target architecture flags")
endif()
if (NOT DEFINED DEVILS_ENGINE_ARCH_FLAGS_GNU)
  set(DEVILS_ENGINE_ARCH_FLAGS_GNU -mavx CACHE STRING "GCC/Clang target architecture flags")
endif()

# --- MSVC runtime library -----------------------------------------------------
# Standard CMake (CMP0091) way to select the dynamic multithreaded runtime:
#   Debug -> /MDd, every other config -> /MD. Set on the parent scope so the
# sibling library/executable targets created after add_subdirectory(libs/options)
# pick it up. Guarded so an external project / toolchain can override it.
if (MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" PARENT_SCOPE)
endif()

# --- compile flags ------------------------------------------------------------
target_compile_options(devils_engine_options INTERFACE
  $<$<CONFIG:Debug>:$<$<CXX_COMPILER_ID:MSVC>:${DEVILS_ENGINE_ARCH_FLAGS_MSVC} /Zi /Wall /wd5045 /wd4820 /wd4514 /GR- /permissive- /Zc:preprocessor>>
  $<$<CONFIG:Release>:$<$<CXX_COMPILER_ID:MSVC>:${DEVILS_ENGINE_ARCH_FLAGS_MSVC} /Wall /wd5045 /wd4820 /O2 /GR- /permissive- /Zc:preprocessor>>
  $<$<CONFIG:MinSizeRel>:$<$<CXX_COMPILER_ID:MSVC>:${DEVILS_ENGINE_ARCH_FLAGS_MSVC} /Wall /wd5045 /wd4820 /O2 /GR- /permissive- /Zc:preprocessor>>
  $<$<CONFIG:RelWithDebInfo>:$<$<CXX_COMPILER_ID:MSVC>:${DEVILS_ENGINE_ARCH_FLAGS_MSVC} /ZI /Wall /wd5045 /wd4820 /O2 /GR- /permissive- /Zc:preprocessor>>

  $<$<CONFIG:Debug>:$<$<CXX_COMPILER_ID:GNU,Clang>:${DEVILS_ENGINE_ARCH_FLAGS_GNU} -g -Wall -Wextra -fno-rtti -fno-inline>>
  $<$<CONFIG:Release>:$<$<CXX_COMPILER_ID:GNU,Clang>:${DEVILS_ENGINE_ARCH_FLAGS_GNU} -O3 -Wall -Wextra -fno-rtti -pipe>>
  $<$<CONFIG:MinSizeRel>:$<$<CXX_COMPILER_ID:GNU,Clang>:${DEVILS_ENGINE_ARCH_FLAGS_GNU} -O3 -Wall -Wextra -fno-rtti -pipe -s>>
  $<$<CONFIG:RelWithDebInfo>:$<$<CXX_COMPILER_ID:GNU,Clang>:${DEVILS_ENGINE_ARCH_FLAGS_GNU} -O3 -g -Wall -Wextra -fno-rtti -pipe>>
)

target_compile_definitions(devils_engine_options INTERFACE
  $<$<CONFIG:Release>:NDEBUG _NDEBUG>
  $<$<CONFIG:MinSizeRel>:NDEBUG _NDEBUG>
  DEVILS_ENGINE_PROJECT_NAME="${PROJECT_NAME}"
  DEVILS_ENGINE_ENGINE_NAME="${DEVILS_ENGINE_NAME}"
)

set(_de_lto_cfg "$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>,$<CONFIG:RelWithDebInfo>>")
set(_de_lto_on "$<AND:$<BOOL:${DEVILS_ENGINE_ENABLE_LTO}>,${_de_lto_cfg}>")

# GCC uses parallel ("auto") whole-program LTO, Clang uses ThinLTO. The same
# flavor must be passed at compile and at link time.
target_compile_options(devils_engine_options INTERFACE
  $<${_de_lto_on}:$<$<CXX_COMPILER_ID:GNU>:-flto=auto>>
  $<${_de_lto_on}:$<$<CXX_COMPILER_ID:Clang>:-flto=thin>>
  $<${_de_lto_on}:$<$<CXX_COMPILER_ID:MSVC>:/GL>>
)
target_link_options(devils_engine_options INTERFACE
  $<${_de_lto_on}:$<$<CXX_COMPILER_ID:GNU>:-flto=auto>>
  $<${_de_lto_on}:$<$<CXX_COMPILER_ID:Clang>:-flto=thin>>
  $<${_de_lto_on}:$<$<CXX_COMPILER_ID:MSVC>:/LTCG>>
)

# --- header-only third-party include directories ------------------------------
# Previously declared as bare include_directories() in the root project. Carried
# here as INTERFACE so only targets that actually link the options pick them up.
# Use the per-package <name>_SOURCE_DIR paths reported by FetchContent_MakeAvailable
# (already populated, since this directory is added afterwards).
target_include_directories(devils_engine_options INTERFACE
  "${dr_libs_SOURCE_DIR}/"
  "${stb_SOURCE_DIR}/"
  "${zpp_bits_SOURCE_DIR}/"
  "${rapidhash_SOURCE_DIR}/"
)
