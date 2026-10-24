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
  src/pkg/metainf.cpp
  src/elf/dynamic.cpp
)
target_include_directories(didjcore PUBLIC src)

add_executable(didjtool tools/didjtool.cpp)
target_link_libraries(didjtool PRIVATE didjcore)

include(CTest)
if(BUILD_TESTING)
  add_executable(didj_tests tests/test_main.cpp)
  target_link_libraries(didj_tests PRIVATE didjcore)
  target_include_directories(didj_tests PRIVATE tests)
  add_test(NAME didj_tests COMMAND didj_tests)
endif()
