# Build configurations for the engine targets.
#
#   Debug    unoptimised, debug CRT, asserts on.
#   Dev      optimised with asserts on and the release CRT (editor default,
#            see docs/design/mt-design.md change 22).
#   Release  optimised, asserts off.
#
# Debug info is embedded (/Z7) on MSVC so parallel Ninja builds never contend
# for a shared PDB.

get_property(_se_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _se_multi_config AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Dev CACHE STRING "Build type" FORCE)
endif()
if(NOT _se_multi_config)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Dev Release)
endif()

if(MSVC)
  set(_se_dev_flags "/O2 /Ob2")
  set(_se_dev_link "/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO")
else()
  set(_se_dev_flags "-O2 -g")
  set(_se_dev_link "")
endif()
foreach(_lang C CXX)
  set(CMAKE_${_lang}_FLAGS_DEV "${_se_dev_flags}"
      CACHE STRING "Flags for the Dev configuration")
endforeach()
foreach(_kind EXE SHARED MODULE)
  set(CMAKE_${_kind}_LINKER_FLAGS_DEV "${_se_dev_link}"
      CACHE STRING "Linker flags for the Dev configuration")
endforeach()
set(CMAKE_STATIC_LINKER_FLAGS_DEV "" CACHE STRING
    "Archiver flags for the Dev configuration")

# Imported targets built in other configurations map Dev onto Release.
set(CMAKE_MAP_IMPORTED_CONFIG_DEV Release RelWithDebInfo "")

if(MSVC)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
      "$<$<CONFIG:Debug,Dev,Release,RelWithDebInfo>:Embedded>")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " /DEBUG /OPT:REF /OPT:ICF")
endif()

# Sanitizers apply to every target added after this point, dependencies
# included, so all objects in a binary agree.
set(SE_SANITIZER "" CACHE STRING "Sanitizer: empty, address or thread")
set_property(CACHE SE_SANITIZER PROPERTY STRINGS "" address thread)
if(SE_SANITIZER STREQUAL "address")
  if(MSVC)
    add_compile_options(/fsanitize=address)
    add_link_options(/INCREMENTAL:NO)
  else()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
  endif()
elseif(SE_SANITIZER STREQUAL "thread")
  if(MSVC)
    message(FATAL_ERROR "ThreadSanitizer is not available with MSVC; use the "
                        "linux-clang-tsan preset in WSL.")
  endif()
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(NOT SE_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unknown SE_SANITIZER '${SE_SANITIZER}'")
endif()
