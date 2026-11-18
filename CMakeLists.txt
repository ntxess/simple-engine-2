# simple-engine-2
#
# The new engine targets (engine/, games/, apps/) grow beside the legacy SFML
# tree (core/, app/), which is kept as a deterministic behaviour reference and
# built only with SE_BUILD_LEGACY=ON. See docs/design/ and docs/decisions.md.

cmake_minimum_required(VERSION 3.31)
project(simple_engine VERSION 2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# The engine does not use C++20 modules; skip dependency scanning (it needs
# clang-scan-deps on Linux and slows every build).
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(SE_BUILD_ENGINE "Build the engine targets" ON)
option(SE_BUILD_PLATFORM "Build the SDL3 platform, renderer, runtime and editor" ON)
option(SE_BUILD_TESTS "Build the tests" ON)
option(SE_BUILD_BENCH "Build se_bench" ON)
option(SE_BUILD_LEGACY "Build the legacy SFML tree (behaviour reference)" OFF)
option(SE_TRACY "Enable Tracy instrumentation" OFF)
option(SE_CRASH "Enable out-of-process crash capture (crashpad)" ON)
option(SE_SDL_PATCH "Apply the engine's SDL3 presentation patch" ON)
option(SE_WARNINGS_AS_ERRORS "Treat warnings as errors in engine targets" OFF)
option(SE_THREAD_SAFETY_ERRORS "Treat -Wthread-safety as errors (clang)" OFF)
option(SE_DEPS_SMOKE "Fetch and build every pinned dependency" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(BuildTypes)
include(Warnings)
include(Dependencies)
include(SeModule)
include(Checks)

enable_testing()

if(SE_BUILD_ENGINE)
  add_subdirectory(engine)
  add_subdirectory(games)
  # M10a: the editor executable (apps/editor).
  if(SE_BUILD_PLATFORM)
    add_subdirectory(apps)
  endif()
  if(SE_BUILD_TESTS)
    add_subdirectory(tests)
  endif()
  if(SE_BUILD_BENCH)
    add_subdirectory(bench)
  endif()
endif()

if(SE_DEPS_SMOKE)
  add_subdirectory(cmake/deps_smoke)
endif()

if(SE_BUILD_LEGACY)
  include(Legacy)
endif()

se_check_link_graph()
if(SE_BUILD_ENGINE AND SE_BUILD_TESTS)
  se_register_checks()
endif()
