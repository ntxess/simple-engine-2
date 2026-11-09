# Pinned third-party dependencies, fetched with FetchContent.
#
# Every archive is pinned by URL and SHA256 and downloaded once into the
# shared cache SE_DEPS_CACHE (.deps/ at the repository root, gitignored).
# Later configures of any preset reuse the cached archive without network
# access. Pins, sizes and reasons are listed in docs/deps.md.
#
# Usage: se_use_deps(<name>...) makes the named dependencies available.

include(FetchContent)

# The SE_DEPS_CACHE environment variable lets git worktrees and WSL clones
# share one archive cache with the main checkout.
if(DEFINED ENV{SE_DEPS_CACHE} AND NOT "$ENV{SE_DEPS_CACHE}" STREQUAL "")
  set(_se_default_cache "$ENV{SE_DEPS_CACHE}")
else()
  set(_se_default_cache "${CMAKE_SOURCE_DIR}/.deps")
endif()
set(SE_DEPS_CACHE "${_se_default_cache}" CACHE PATH
    "Shared download cache for pinned dependency archives")
set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)

# Records a pin. The archive is downloaded (and verified) into the shared cache
# only when the dependency is first used, and FetchContent then extracts the
# cached file, so no build tree downloads anything a second time.
function(_se_declare name url sha256)
  set_property(GLOBAL PROPERTY SE_DEP_${name}_URL "${url}")
  set_property(GLOBAL PROPERTY SE_DEP_${name}_SHA256 "${sha256}")
  set_property(GLOBAL PROPERTY SE_DEP_${name}_ARGS "${ARGN}")
endfunction()

function(_se_fetch_archive name)
  get_property(_url GLOBAL PROPERTY SE_DEP_${name}_URL)
  get_property(_sha GLOBAL PROPERTY SE_DEP_${name}_SHA256)
  get_property(_args GLOBAL PROPERTY SE_DEP_${name}_ARGS)
  if(NOT _url)
    message(FATAL_ERROR "se_use_deps: unknown dependency '${name}'")
  endif()
  get_filename_component(_file "${_url}" NAME)
  set(_cached "${SE_DEPS_CACHE}/${name}/${_file}")
  if(EXISTS "${_cached}")
    file(SHA256 "${_cached}" _have)
    if(NOT _have STREQUAL _sha)
      message(WARNING "Cached ${_cached} has the wrong hash; downloading again")
      file(REMOVE "${_cached}")
    endif()
  endif()
  if(NOT EXISTS "${_cached}")
    message(STATUS "Downloading pinned ${name}: ${_url}")
    file(DOWNLOAD "${_url}" "${_cached}.part"
         EXPECTED_HASH SHA256=${_sha} STATUS _status TLS_VERIFY ON)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
      file(REMOVE "${_cached}.part")
      message(FATAL_ERROR "Download of ${name} failed: ${_status}")
    endif()
    file(RENAME "${_cached}.part" "${_cached}")
  endif()
  FetchContent_Declare(${name}
    URL "${_cached}"
    URL_HASH SHA256=${_sha}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
    ${_args})
endfunction()

_se_declare(catch2
  https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.1.tar.gz
  d16bd1b8b2364918bd472cb216394acb93bde6784890df0d91bccfb3f28d777d)
_se_declare(spdlog
  https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz
  d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744)
_se_declare(glaze
  https://github.com/stephenberry/glaze/archive/refs/tags/v9.1.0.tar.gz
  55f8a675397c505f9bee6c160e1c26b0085868f185d04d7a7727531fdbff4bd4)
_se_declare(tracy
  https://github.com/wolfpld/tracy/archive/refs/tags/v0.14.1.tar.gz
  bf4af567e9c7524d07f3caa745fad02fb33bd5694f11910750382d1efbb251c1)
_se_declare(sentry
  https://github.com/getsentry/sentry-native/releases/download/0.17.6/sentry-native.zip
  40b4b70cbada5bfae5b73f8267831dd023ec676d9676fb0d890878f31d2b5055)
_se_declare(miniaudio
  https://github.com/mackron/miniaudio/archive/refs/tags/0.11.26.tar.gz
  838ba55a21c9142adf7b76bdc4351e5cd8b7ae128f46b25ec25f94ae9969c600)
_se_declare(lz4
  https://github.com/lz4/lz4/releases/download/v1.10.0/lz4-1.10.0.tar.gz
  537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
  SOURCE_SUBDIR build/cmake)
_se_declare(imgui
  https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0-docking.tar.gz
  746c265540bd3aa109570f29492ef4d8f5b421dd9fd0049c35ca35f21496f1a5)

# SDL3 carries the engine's presentation patch (M6) when SE_SDL_PATCH is on
# and the patch file exists.
set(_se_sdl_patch
    "${CMAKE_SOURCE_DIR}/cmake/patches/sdl3-3.4.20-se-presentation.patch")
set(_se_sdl_patch_args)
if(SE_SDL_PATCH AND EXISTS "${_se_sdl_patch}")
  file(SHA256 "${_se_sdl_patch}" _se_sdl_patch_sha)
  set(_se_sdl_patch_args PATCH_COMMAND
      "${CMAKE_COMMAND}" -DPATCH_FILE=${_se_sdl_patch}
      -DPATCH_SHA256=${_se_sdl_patch_sha}
      -P "${CMAKE_SOURCE_DIR}/cmake/ApplyPatch.cmake")
endif()
_se_declare(sdl3
  https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
  f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f
  ${_se_sdl_patch_args})

# EnTT stays vendored: the design was verified against 3.16.0.
# ENTT_USE_ATOMIC must be identical in every translation unit.
add_library(se_entt INTERFACE)
add_library(se::entt ALIAS se_entt)
target_include_directories(se_entt SYSTEM INTERFACE
  "${CMAKE_SOURCE_DIR}/dep/entt-3.16.0/src")
target_compile_definitions(se_entt INTERFACE ENTT_USE_ATOMIC)

# Options are set right before each dependency is made available.
macro(_se_options_catch2)
  set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
endmacro()
macro(_se_options_spdlog)
  set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
  set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
  set(SPDLOG_DISABLE_DEFAULT_LOGGER ON CACHE BOOL "" FORCE)
endmacro()
macro(_se_options_glaze)
  set(glaze_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
endmacro()
macro(_se_options_tracy)
  set(TRACY_ENABLE ${SE_TRACY} CACHE BOOL "" FORCE)
  set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
  set(TRACY_ONLY_LOCALHOST ON CACHE BOOL "" FORCE)
  set(TRACY_NO_BROADCAST ON CACHE BOOL "" FORCE)
endmacro()
macro(_se_options_sentry)
  set(SENTRY_BACKEND crashpad CACHE STRING "" FORCE)
  set(SENTRY_TRANSPORT none CACHE STRING "" FORCE)
  set(SENTRY_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(SENTRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(SENTRY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
endmacro()
macro(_se_options_miniaudio)
  set(MINIAUDIO_INSTALL OFF CACHE BOOL "" FORCE)
  set(MINIAUDIO_NO_LIBVORBIS ON CACHE BOOL "" FORCE)
  set(MINIAUDIO_NO_LIBOPUS ON CACHE BOOL "" FORCE)
endmacro()
macro(_se_options_lz4)
  set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
  set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
endmacro()
macro(_se_options_sdl3)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  # Linux without window-system packages (the WSL presets, M5): SDL's dummy
  # and offscreen video drivers only, and no system libraries SDL would look
  # for. SE_SDL_WINDOW_SYSTEMS=ON restores SDL's own detection (M18).
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT SE_SDL_WINDOW_SYSTEMS)
    foreach(_se_sdl_off X11 WAYLAND KMSDRM OPENGL OPENGLES RPI ROCKCHIP VIVANTE
            IBUS DBUS LIBUDEV LIBURING HIDAPI_LIBUSB ALSA JACK PIPEWIRE
            PULSEAUDIO SNDIO OSS)
      set(SDL_${_se_sdl_off} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(SDL_OFFSCREEN ON CACHE BOOL "" FORCE)
    set(SDL_DUMMYVIDEO ON CACHE BOOL "" FORCE)
    # SDL refuses to configure without X11 or Wayland unless told this is
    # intended.
    set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
  endif()
endmacro()
option(SE_SDL_WINDOW_SYSTEMS
  "Linux: let SDL3 build its X11, Wayland and KMSDRM drivers (needs their dev packages)"
  OFF)
macro(_se_options_imgui)
endmacro()

# se_use_deps(<name>...): configure and make the named dependencies
# available. Names: catch2 spdlog glaze tracy sentry miniaudio lz4 imgui sdl3.
macro(se_use_deps)
  foreach(_se_dep ${ARGN})
    get_property(_se_done GLOBAL PROPERTY SE_DEP_${_se_dep}_DONE)
    if(NOT _se_done)
      _se_fetch_archive(${_se_dep})
      cmake_language(CALL _se_options_${_se_dep})
      FetchContent_MakeAvailable(${_se_dep})
      set_property(GLOBAL PROPERTY SE_DEP_${_se_dep}_DONE TRUE)
    endif()
  endforeach()
endmacro()
