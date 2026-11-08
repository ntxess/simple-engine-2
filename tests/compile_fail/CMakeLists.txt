# Compile-fail fixtures, configured and built by CTest (tests/CMakeLists.txt)
# in their own build tree, so the deliberately broken translation unit never
# reaches the main build or its compile database.
#
#   cmake -S tests/compile_fail -B <dir> -DSE_REPO_ROOT=<repo>

cmake_minimum_required(VERSION 3.31)
project(se_compile_fail LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT SE_REPO_ROOT)
  message(FATAL_ERROR "pass -DSE_REPO_ROOT=<repository root>")
endif()

function(se_compile_fail_target name fails)
  add_library(${name} OBJECT chunk_create.cc)
  target_include_directories(${name} PRIVATE
    "${SE_REPO_ROOT}/engine/se_core/include")
  target_include_directories(${name} SYSTEM PRIVATE
    "${SE_REPO_ROOT}/dep/entt-3.16.0/src")
  target_compile_definitions(${name} PRIVATE
    ENTT_USE_ATOMIC SE_ASSERTS=1 SE_COMPILE_FAIL=${fails})
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(${name} PRIVATE
      /EHsc /permissive- /utf-8 -Wthread-safety -Werror=thread-safety)
  else()
    target_compile_options(${name} PRIVATE
      -Wthread-safety -Werror=thread-safety)
  endif()
endfunction()

se_compile_fail_target(chunk_create_fails 1)
se_compile_fail_target(chunk_create_control 0)
