# Cross toolchain for the AArch64 libretro core.
#
# The defaults tune for the H700 (Cortex-A53, ARMv8-A); they are cache variables
# so another SoC can reuse this file without editing it:
#
#   cmake -S yabause/src/libretro -B build-aarch64 \
#         -DCMAKE_TOOLCHAIN_FILE=yabause/src/libretro/toolchains/aarch64-linux.cmake \
#         -DYAB_MARCH=armv8.2-a -DYAB_MTUNE=cortex-a55
#
# H700_DEPS must point at a directory with the GLES/EGL headers and the
# libEGL/libmali entries; the CI job generates stubs for the two libraries and
# the device supplies the real ones at run time.
#
# A local development build can instead point at a harvested device sysroot with
# the vendor libraries (H700_TOOLCHAIN / H700_DEPS) so nothing has to be stubbed.

if(NOT YAB_MARCH)
  set(YAB_MARCH "armv8-a+crc")
endif()
if(NOT YAB_MTUNE)
  set(YAB_MTUNE "cortex-a53")
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(YAB_ARCH_FLAGS "-O3 -fno-strict-aliasing -march=${YAB_MARCH} -mtune=${YAB_MTUNE}")

set(CMAKE_C_FLAGS_INIT   "${YAB_ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${YAB_ARCH_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${YAB_ARCH_FLAGS}")

# Keep the GL entries as DT_NEEDED even though only stubs are linked here.
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-Wl,--no-as-needed")
