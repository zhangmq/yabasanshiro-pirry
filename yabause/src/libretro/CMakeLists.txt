# libretro core — independent CMake project (offline dev build).
# Generated from the standalone build's object list; see .notes/CURRENT/BUILD-CORE.md.
cmake_minimum_required(VERSION 3.16)
project(yabasanshiro_libretro C CXX ASM)

set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE)
   set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Dependencies: the SAME sources and pins the standalone uses, so the two builds
# can never drift apart:
#   libchdr  devmiyax/libchdr @ 074ff1614f2a685f2b5a95b0e788bff6297d5680
#            (yabause/CMake/Packages/external_libchdr.cmake GIT_TAG)
#   zlib     madler/zlib      @ 50893291621658f355bc5b4d450a8d06a563053d  (zlib 1.2.8,
#            yabause/CMake/Packages/external_zlib.cmake ZLIB_TAG)
#   musashi  generated m68kops*.c from the standalone's m68kmake run
# They are checked out into YAB_DEPS_CACHE (daily dev: reused, no network) and
# built here from source; `build-libretro.sh --release` starts from a fresh cache
# so everything is fetched online.
set(YAB_DEPS_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/../../../.notes/deps" CACHE PATH "dependency source cache")
set(LIBCHDR_ROOT "${YAB_DEPS_CACHE}/libchdr")
set(LIBCHDR_SRC  "${LIBCHDR_ROOT}/src")
set(ZLIB_ROOT    "${YAB_DEPS_CACHE}/zlib")
set(MUSASHI_GEN  "${YAB_DEPS_CACHE}/musashi")

if(NOT DEFINED H700_DEPS)
  if(DEFINED DEPS)
    set(H700_DEPS ${DEPS})
  elseif(DEFINED ENV{H700_DEPS})
    set(H700_DEPS $ENV{H700_DEPS})
  endif()
endif()
set(ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Build both dependencies from source in this tree: the core is a shared object,
# so everything it links must be PIC (the standalone's archives are not).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT EXISTS "${LIBCHDR_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR "libchdr sources missing at ${LIBCHDR_ROOT} (run .notes/tools/build-libretro.sh)")
endif()
if(NOT EXISTS "${ZLIB_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR "zlib sources missing at ${ZLIB_ROOT} (run .notes/tools/build-libretro.sh)")
endif()
add_subdirectory(${LIBCHDR_ROOT} ${CMAKE_BINARY_DIR}/libchdr)

# zlib is built as an ExternalProject in its own tree, exactly like the
# standalone does it: the engine must link zlib 1.2.8, while libchdr brings its
# own bundled copy for the CHD codecs. Doing it in-tree would clash on the
# "zlib" target name.
include(ExternalProject)
if(CMAKE_CROSSCOMPILING)
  get_filename_component(YAB_TCHAIN "${CMAKE_TOOLCHAIN_FILE}" REALPATH BASE_DIR "${CMAKE_BINARY_DIR}")
  set(YAB_EP_TOOLCHAIN -DCMAKE_TOOLCHAIN_FILE=${YAB_TCHAIN} -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM})
else()
  set(YAB_EP_TOOLCHAIN "")
endif()
set(ZLIB_INSTALL ${CMAKE_BINARY_DIR}/zlib/install)
ExternalProject_Add(zlib_ep
  NAME zlib   # libchdr already owns a "zlib" target
  SOURCE_DIR ${ZLIB_ROOT}
  DOWNLOAD_COMMAND ""
  UPDATE_COMMAND ""
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${ZLIB_INSTALL}
             -DCMAKE_BUILD_TYPE=Release
             -DCMAKE_POSITION_INDEPENDENT_CODE=ON
             ${YAB_EP_TOOLCHAIN}
  BUILD_BYPRODUCTS ${ZLIB_INSTALL}/lib/libz.a)

set(ENGINE_SOURCES
   ${ENGINE_DIR}/BackupManager.cpp
   ${ENGINE_DIR}/Counter.cpp
   ${ENGINE_DIR}/PlayRecorder.cpp
   ${ENGINE_DIR}/bios.c
   ${ENGINE_DIR}/cd-linux.c
   ${ENGINE_DIR}/cd-web.cpp
   ${ENGINE_DIR}/cdbase.c
   ${ENGINE_DIR}/cheat.c
   ${ENGINE_DIR}/coffelf.c
   ${ENGINE_DIR}/cs0.c
   ${ENGINE_DIR}/cs1.c
   ${ENGINE_DIR}/cs2.c
   ${ENGINE_DIR}/debug.c
   ${ENGINE_DIR}/error.c
   ${ENGINE_DIR}/frameprofile.cpp
   ${ENGINE_DIR}/gameinfo.c
   ${ENGINE_DIR}/japmodem.c
   ${ENGINE_DIR}/jsoncpp.cpp
   ${ENGINE_DIR}/m68kcore.c
   ${ENGINE_DIR}/m68kd.c
   ${ENGINE_DIR}/m68kmusashi.c
   ${ENGINE_DIR}/memory.c
   ${ENGINE_DIR}/movie.c
   ${ENGINE_DIR}/musashi/m68kcpu.c
   ${ENGINE_DIR}/musashi/m68kdasm.c
   ${ENGINE_DIR}/nanovg/nanovg.c
   ${ENGINE_DIR}/nanovg/nanovg_osdcore.c
   ${ENGINE_DIR}/netlink.c
   ${ENGINE_DIR}/osdcore.c
   ${ENGINE_DIR}/peripheral.c
   ${ENGINE_DIR}/profile.c
   ${ENGINE_DIR}/scsp.c
   ${ENGINE_DIR}/scspdsp.c
   ${ENGINE_DIR}/scu.c
   ${ENGINE_DIR}/sh2_dynarec_devmiyax/DynarecSh2.cpp
   ${ENGINE_DIR}/sh2_dynarec_devmiyax/DynarecSh2CInterface.cpp
   ${ENGINE_DIR}/sh2_dynarec_devmiyax/dynalib_arm64.s
   ${ENGINE_DIR}/sh2cache.c
   ${ENGINE_DIR}/sh2core.c
   ${ENGINE_DIR}/sh2d.c
   ${ENGINE_DIR}/sh2iasm.c
   ${ENGINE_DIR}/sh2idle.c
   ${ENGINE_DIR}/sh2int.c
   ${ENGINE_DIR}/sh2trace.c
   ${ENGINE_DIR}/smpc.c
   ${ENGINE_DIR}/stv/stv.c
   ${ENGINE_DIR}/stv/junzip.c
   ${ENGINE_DIR}/stv/eeprom.c
   ${ENGINE_DIR}/stv/decrypt.c
   ${ENGINE_DIR}/stv/stvcomp.c
   ${ENGINE_DIR}/snddummy.c
   ${ENGINE_DIR}/sock-linux.c
   ${ENGINE_DIR}/state_save.cpp
   ${ENGINE_DIR}/thr-linux.cpp
   ${ENGINE_DIR}/titan/titan.c
   ${ENGINE_DIR}/vdp1.cpp
   ${ENGINE_DIR}/vdp2debug.c
   ${ENGINE_DIR}/vidogl.c
   ${ENGINE_DIR}/vidshared.c
   ${ENGINE_DIR}/vidsoft.c
   ${ENGINE_DIR}/yabause.c
   ${ENGINE_DIR}/ygl_texture.cpp
   ${ENGINE_DIR}/yglcache.c
   ${ENGINE_DIR}/ygles.c
   ${ENGINE_DIR}/yglshaderes.c
   ${MUSASHI_GEN}/m68kopac.c
   ${MUSASHI_GEN}/m68kopdm.c
   ${MUSASHI_GEN}/m68kopnz.c
   ${MUSASHI_GEN}/m68kops.c

)

add_library(yabasanshiro_libretro SHARED
   libretro.c
   engine/vdp2.cpp
   yui_libretro.c
   sndlibretro.c
   log.c
   port_stubs.c
   ${ENGINE_SOURCES}
)

target_include_directories(yabasanshiro_libretro PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${ENGINE_DIR}
   ${ENGINE_DIR}/stv
   ${ENGINE_DIR}/nanovg
   ${ENGINE_DIR}/musashi
   ${MUSASHI_GEN}
   ${LIBCHDR_SRC}
   ${ZLIB_ROOT}
   ${ZLIB_INSTALL}/include
   ${H700_DEPS}/include
)

target_compile_definitions(yabasanshiro_libretro PRIVATE
   AARCH64 ARCH_IS_LINUX=1 DYNAREC_DEVMIYAX=1 GCC_VERSION=12
   HAVE_BUILTIN_BSWAP16=1 HAVE_BUILTIN_BSWAP32=1 HAVE_C99_VARIADIC_MACROS=1
   HAVE_FLOORF=1 HAVE_GETTIMEOFDAY=1 HAVE_LIBGL=1 HAVE_MUSASHI=1
   HAVE_STDINT_H=1 HAVE_STRCASECMP=1 HAVE_SYS_TIME_H=1 IMPROVED_SAVESTATES
   SH2_TRACE=1 YAB_ASYNC_RENDERING YAB_PORT_OSD _OGLES3_
   PACKAGE="yabause" VERSION="1.6.2"
   __LIBRETRO__ LOG_ENABLED
)

target_compile_options(yabasanshiro_libretro PRIVATE -O3 -fomit-frame-pointer -fno-strict-aliasing -Wno-format-truncation -Wno-format-overflow -Wno-stringop-overflow)
set_target_properties(yabasanshiro_libretro PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_dependencies(yabasanshiro_libretro zlib_ep)

target_link_libraries(yabasanshiro_libretro PRIVATE
   chdr-static flac-static lzma-static crypto-static ${ZLIB_INSTALL}/lib/libz.a
   ${H700_DEPS}/lib/libEGL.so.1
   ${H700_DEPS}/lib/libmali.so.0
   pthread dl m)

set_target_properties(yabasanshiro_libretro PROPERTIES PREFIX "" OUTPUT_NAME "yabasanshiro_libretro")
