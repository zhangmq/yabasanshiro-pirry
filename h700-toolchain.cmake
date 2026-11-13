# Cross toolchain for the H700 family (baseOS: glibc 2.35, aarch64).
#
# Paths are parameters, not baked in:
#   ./build-libretro.sh --toolchain h700-toolchain.cmake
#     (with H700_TOOLCHAIN=/path/to/h700-toolchain H700_DEPS=/path/to/h700-deps)
#   or: cmake -S yabause -B build -DCMAKE_TOOLCHAIN_FILE=h700-toolchain.cmake \
#             -DTC=/path/to/h700-toolchain -DDEPS=/path/to/h700-deps
# (TC must contain root/ = sysroot and bin/ = the toolchain's binutils;
#  DEPS must contain include/ and lib/ with the device GL/EGL + libmali.)
# TC / DEPS can come from -D or from the environment.  The environment form is
# required for try_compile() sub-projects: CMake re-reads this file there but
# does not propagate custom -D cache entries, so -D alone makes the compiler
# ABI check fail with "pass -DTC=...".
if(NOT DEFINED TC AND DEFINED ENV{H700_TOOLCHAIN})
  set(TC "$ENV{H700_TOOLCHAIN}")
endif()
if(NOT DEFINED DEPS AND DEFINED ENV{H700_DEPS})
  set(DEPS "$ENV{H700_DEPS}")
endif()
if(NOT DEFINED TC)
  message(FATAL_ERROR "h700-toolchain.cmake: pass -DTC=/path/to/h700-toolchain or set H700_TOOLCHAIN")
endif()
if(NOT DEFINED DEPS)
  message(FATAL_ERROR "h700-toolchain.cmake: pass -DDEPS=/path/to/h700-deps or set H700_DEPS")
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   ${TC}/root/usr/bin/aarch64-linux-gnu-gcc-12)
set(CMAKE_CXX_COMPILER ${TC}/root/usr/bin/aarch64-linux-gnu-g++-12)
set(CMAKE_ASM_COMPILER ${TC}/root/usr/bin/aarch64-linux-gnu-gcc-12)
set(CMAKE_ASM_FLAGS_INIT "-B${TC}/bin -march=armv8-a+crc+fp+simd -mcpu=cortex-a53 -mtune=cortex-a53")
set(CMAKE_AR           ${TC}/root/usr/bin/ar)
set(CMAKE_RANLIB       ${TC}/root/usr/bin/ranlib)
set(CMAKE_STRIP        ${TC}/root/usr/bin/strip)

set(CMAKE_SYSROOT ${TC}/root)

set(CMAKE_C_FLAGS_INIT   "-B${TC}/bin -O3 -fno-strict-aliasing -D_GNU_SOURCE -march=armv8-a+crc+fp+simd -mcpu=cortex-a53 -mtune=cortex-a53 -I${DEPS}/include -I${DEPS}/include/SDL2")
set(CMAKE_CXX_FLAGS_INIT "-B${TC}/bin -O3 -fno-strict-aliasing -D_GNU_SOURCE -march=armv8-a+crc+fp+simd -mcpu=cortex-a53 -mtune=cortex-a53 -I${DEPS}/include -I${DEPS}/include/SDL2")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-B${TC}/bin -L${DEPS}/lib -Wl,-rpath,\$ORIGIN -Wl,--allow-multiple-definition -Wl,--allow-shlib-undefined")

set(CMAKE_FIND_ROOT_PATH ${TC}/root ${DEPS})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
