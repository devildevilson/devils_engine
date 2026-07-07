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
# Normally defined by the root CMakeLists before dependencies are configured.
# Kept here as a standalone fallback for embedding just libs/options.
if (NOT DEFINED DEVILS_ENGINE_ARCH)
  set(DEVILS_ENGINE_ARCH "AVX" CACHE STRING "Target ISA baseline: OFF, AVX, AVX2, NATIVE")
  set_property(CACHE DEVILS_ENGINE_ARCH PROPERTY STRINGS OFF AVX AVX2 NATIVE)
endif()
if (NOT DEFINED DEVILS_ENGINE_ARCH_FLAGS_AUTO)
  option(DEVILS_ENGINE_ARCH_FLAGS_AUTO "Derive compiler architecture flags from DEVILS_ENGINE_ARCH" ON)
endif()

if (DEVILS_ENGINE_ARCH_FLAGS_AUTO OR NOT DEFINED DEVILS_ENGINE_ARCH_FLAGS_MSVC OR NOT DEFINED DEVILS_ENGINE_ARCH_FLAGS_GNU)
  set(_DEVILS_ENGINE_ARCH_FLAGS_MSVC "")
  set(_DEVILS_ENGINE_ARCH_FLAGS_GNU "")

  if (DEVILS_ENGINE_ARCH STREQUAL "AVX")
    set(_DEVILS_ENGINE_ARCH_FLAGS_MSVC /arch:AVX)
    set(_DEVILS_ENGINE_ARCH_FLAGS_GNU -mavx)
  elseif (DEVILS_ENGINE_ARCH STREQUAL "AVX2")
    set(_DEVILS_ENGINE_ARCH_FLAGS_MSVC /arch:AVX2)
    set(_DEVILS_ENGINE_ARCH_FLAGS_GNU -mavx2)
  elseif (DEVILS_ENGINE_ARCH STREQUAL "NATIVE")
    set(_DEVILS_ENGINE_ARCH_FLAGS_GNU -march=native)
  elseif (NOT DEVILS_ENGINE_ARCH STREQUAL "OFF")
    message(FATAL_ERROR "Unsupported DEVILS_ENGINE_ARCH='${DEVILS_ENGINE_ARCH}'. Expected OFF, AVX, AVX2, or NATIVE.")
  endif()

  if (DEVILS_ENGINE_ARCH_FLAGS_AUTO OR NOT DEFINED DEVILS_ENGINE_ARCH_FLAGS_MSVC)
    set(DEVILS_ENGINE_ARCH_FLAGS_MSVC ${_DEVILS_ENGINE_ARCH_FLAGS_MSVC} CACHE STRING "MSVC target architecture flags" FORCE)
  endif()
  if (DEVILS_ENGINE_ARCH_FLAGS_AUTO OR NOT DEFINED DEVILS_ENGINE_ARCH_FLAGS_GNU)
    set(DEVILS_ENGINE_ARCH_FLAGS_GNU ${_DEVILS_ENGINE_ARCH_FLAGS_GNU} CACHE STRING "GCC/Clang target architecture flags" FORCE)
  endif()
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
  $<$<STREQUAL:${DEVILS_ENGINE_ARCH},AVX>:GLM_FORCE_AVX>
  $<$<STREQUAL:${DEVILS_ENGINE_ARCH},AVX2>:GLM_FORCE_AVX2>
  $<$<STREQUAL:${DEVILS_ENGINE_ARCH},NATIVE>:GLM_FORCE_INTRINSICS>
  $<$<STREQUAL:${DEVILS_ENGINE_ARCH},OFF>:GLM_FORCE_PURE>
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
  "${reflect_SOURCE_DIR}/"
)
