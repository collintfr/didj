cmake_minimum_required(VERSION 3.16)
project(didj LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra)
endif()

# Core library shared by the emulator and the tools.
add_library(didjcore STATIC
  src/fs/filesystem.cpp
  src/fs/fat.cpp
  src/fs/jffs2.cpp
  src/util/inflate.cpp
  src/util/md5.cpp
  src/util/png.cpp
  src/pkg/metainf.cpp
  src/pkg/paths.cpp
  src/elf/dynamic.cpp
)
target_include_directories(didjcore PUBLIC src)

option(DIDJ_BUILD_EMULATOR "Build the emulator (fetches Unicorn)" ON)

if(DIDJ_BUILD_EMULATOR)
  include(FetchContent)
  # Unicorn 2.1.1, ARM target only. Set FETCHCONTENT_SOURCE_DIR_UNICORN to
  # build from a local checkout instead.
  set(UNICORN_ARCH "arm" CACHE STRING "" FORCE)
  set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(unicorn
    GIT_REPOSITORY https://github.com/unicorn-engine/unicorn.git
    GIT_TAG d568885d64c89db5b9a722f0c1bef05aa92f84ca  # 2.1.1
    GIT_SHALLOW FALSE
    PATCH_COMMAND ${CMAKE_COMMAND} -DUNICORN_SOURCE_DIR=<SOURCE_DIR> -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_unicorn.cmake
  )
  FetchContent_MakeAvailable(unicorn)

  add_library(didjemu STATIC
    src/emu/cpu.cpp
    src/emu/dev/basic.cpp
    src/emu/dev/board.cpp
    src/emu/dev/input.cpp
    src/emu/dev/lf1000_display.cpp
    src/emu/dev/oss.cpp
    src/emu/hle/gles.cpp
    src/emu/hle/raster.cpp
    src/emu/hle/stub_elf.cpp
    src/emu/loader.cpp
    src/emu/memory.cpp
    src/emu/process.cpp
    src/emu/socket.cpp
    src/emu/syscalls.cpp
    src/emu/vfs.cpp
  )
  target_link_libraries(didjemu PUBLIC didjcore unicorn)

  add_executable(didj-run tools/didj_run.cpp)
  target_link_libraries(didj-run PRIVATE didjemu)
endif()

add_executable(didjtool tools/didjtool.cpp)
target_link_libraries(didjtool PRIVATE didjcore)

add_executable(didj-fetch tools/didj_fetch.cpp)
target_link_libraries(didj-fetch PRIVATE didjcore)

include(CTest)
if(BUILD_TESTING)
  add_executable(didj_tests tests/test_main.cpp)
  target_link_libraries(didj_tests PRIVATE didjcore)
  target_include_directories(didj_tests PRIVATE tests)
  add_test(NAME didj_tests COMMAND didj_tests)
  if(DIDJ_BUILD_EMULATOR)
    add_executable(didj_emu_tests tests/test_emu.cpp)
    target_link_libraries(didj_emu_tests PRIVATE didjemu)
    add_test(NAME didj_emu_tests COMMAND didj_emu_tests)
  endif()
endif()
