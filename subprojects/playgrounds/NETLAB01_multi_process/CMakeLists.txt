cmake_minimum_required(VERSION 3.30)

project(NETLAB01_multi_process VERSION 0.1.0 LANGUAGES CXX)

# The stand needs the real backend, so it exists only where the GNS adapter does.
if (NOT TARGET devils_engine::network_gns)
  return()
endif()

add_executable(NETLAB01_multi_process main.cpp)
target_link_libraries(NETLAB01_multi_process PRIVATE
  devils_engine::network_gns
  devils_engine::network
  devils_engine::utils
  devils_engine::options)

set_target_properties(NETLAB01_multi_process PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/NETLAB01_multi_process/bin")

# This target exists to be CARRIED to machines which cannot build, so its link
# profile is part of what it is rather than a packaging afterthought.
#
#  --as-needed  drops DT_NEEDED entries nothing references. Without it the
#               artifact declares libraries it never calls (libzstd, reached
#               only through an unused corner of devils_utils), and every such
#               entry is one more thing a target machine must happen to have.
#
# What stays DYNAMIC, deliberately: glibc, libstdc++/libgcc, and OpenSSL's
# libcrypto (which GameNetworkingSockets requires for AES-GCM/SHA-256). We do
# not control how a distribution builds those and do not want to: the target's
# own copies are built for the target's own CPU baseline.
#
# Statically linking the C++ runtime was tried and is WRONG here, for a reason
# worth keeping: a static libstdc++ carries the BUILD HOST's ISA choices into
# the artifact. This host is CachyOS, whose packages are built for x86-64-v3,
# so its libstdc++.a holds ~18000 unconditional AVX instructions with no
# runtime dispatch — an artifact linking it cannot run on a CPU without AVX no
# matter what DEVILS_ENGINE_ARCH says about our own code.
if (UNIX AND NOT APPLE)
  target_link_options(NETLAB01_multi_process PRIVATE -Wl,--as-needed)
endif()

# The ISA-level marker, and why it has to be removed rather than trusted.
#
# `ld.so` refuses a binary whose `GNU_PROPERTY_X86_ISA_1_NEEDED` exceeds the
# CPU, with "CPU ISA level is lower than required". That marker is NOT computed
# from the instructions in the binary: the linker propagates it from the shared
# libraries it links against, and on a distribution whose libc is built for
# x86-64-v3 (this host runs CachyOS) every binary linked here inherits a v3
# requirement. Measured: a plain `int main(){return 0;}` compiled here reports
# `ISA needed: x86-64-baseline, x86-64-v2, x86-64-v3` while its own
# `ISA used: x86-64-baseline`. The requirement is a property of the build host,
# not of this artifact, and on the target machine the target's own baseline libc
# is what gets loaded.
#
# The intended fix is `-Wl,-z,x86-64-baseline`, which sets the level explicitly.
# It is unusable here: binutils 2.47 aborts with an internal error in
# `_bfd_x86_elf_merge_gnu_properties`. So the note is dropped after linking.
#
# What that costs: this artifact's note carries only the ISA markers and
# `feature used: x86, x87, XMM` — no CET (IBT/SHSTK) properties, checked. If
# CET is ever enabled for this project, THIS STEP MUST BE REPLACED by lowering
# the ISA level rather than stripping the note, or the binary silently stops
# opting into CET.
if (UNIX AND NOT APPLE AND CMAKE_OBJCOPY)
  add_custom_command(TARGET NETLAB01_multi_process POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} --remove-section=.note.gnu.property
            $<TARGET_FILE:NETLAB01_multi_process>
    COMMENT "Dropping the build host's inherited x86 ISA-level marker")
endif()

if (DEVILS_ENGINE_BUILD_TESTS)
  add_test(NAME NETLAB01_multi_process_verify
    COMMAND NETLAB01_multi_process --verify --quiet)
  set_tests_properties(NETLAB01_multi_process_verify PROPERTIES LABELS devils_engine)
endif()
