# se_add_module(<name> KIND <STATIC|INTERFACE|EXECUTABLE>
#   [SOURCES <file>...]
#   [PUBLIC_DEPS <target>...] [PRIVATE_DEPS <target>...]
#   [FORBID <target>...]          targets that must never be in the link
#                                 closure (checked by se_check_link_graph)
#   [BANNED_INCLUDES <regex>...]  include paths the module may not use
#                                 (checked by tools/checks/check_includes.py)
#   [DETERMINISTIC]               simulation code: determinism flags, and the
#                                 determinism bans of the API scan apply
#   [TEST])                       test or tool code: exempt from the API scan
#                                 (it may use the CRT maths, clocks and throw)
#
# Layout: <dir>/include/<name>/*.h is the public interface, <dir>/src the
# implementation. Every module links se_warnings privately.

set_property(GLOBAL PROPERTY SE_MODULES "")
set_property(GLOBAL PROPERTY SE_CHECK_ENTRIES "")

function(se_add_module name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "DETERMINISTIC;TEST" "KIND"
    "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS;FORBID;BANNED_INCLUDES")
  if(arg_KIND STREQUAL "STATIC")
    add_library(${name} STATIC ${arg_SOURCES})
    set(_scope PUBLIC)
  elseif(arg_KIND STREQUAL "INTERFACE")
    add_library(${name} INTERFACE)
    set(_scope INTERFACE)
  elseif(arg_KIND STREQUAL "EXECUTABLE")
    add_executable(${name} ${arg_SOURCES})
    set(_scope PRIVATE)
  else()
    message(FATAL_ERROR "se_add_module(${name}): unknown KIND '${arg_KIND}'")
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_include_directories(${name} ${_scope}
      "${CMAKE_CURRENT_SOURCE_DIR}/include")
  endif()
  if(NOT arg_KIND STREQUAL "INTERFACE")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src")
      target_include_directories(${name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    endif()
    target_link_libraries(${name} PRIVATE se_warnings)
    if(arg_DETERMINISTIC)
      target_link_libraries(${name} PRIVATE se_determinism)
    endif()
    target_link_libraries(${name} PUBLIC ${arg_PUBLIC_DEPS})
    target_link_libraries(${name} PRIVATE ${arg_PRIVATE_DEPS})
    if(arg_KIND STREQUAL "EXECUTABLE")
      set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    endif()
  else()
    target_link_libraries(${name} INTERFACE ${arg_PUBLIC_DEPS})
  endif()

  set_property(TARGET ${name} PROPERTY SE_FORBID "${arg_FORBID}")
  set_property(GLOBAL APPEND PROPERTY SE_MODULES ${name})

  set(_det false)
  if(arg_DETERMINISTIC)
    set(_det true)
  endif()
  set(_kind engine)
  if(arg_TEST)
    set(_kind test)
  endif()
  set(_banned "")
  foreach(_re ${arg_BANNED_INCLUDES})
    string(REPLACE "\\" "\\\\" _re "${_re}")
    list(APPEND _banned "\"${_re}\"")
  endforeach()
  list(JOIN _banned ", " _banned)
  set_property(GLOBAL APPEND PROPERTY SE_CHECK_ENTRIES
    "{\"name\": \"${name}\", \"dir\": \"${CMAKE_CURRENT_SOURCE_DIR}\", \"kind\": \"${_kind}\", \"deterministic\": ${_det}, \"banned_includes\": [${_banned}]}")
endfunction()

# Banned include patterns shared by modules that must stay headless.
set(SE_BAN_PLATFORM_INCLUDES
  "^SDL3?/" "^imgui" "^backends/imgui" "^windows\\.h$" "^Windows\\.h$"
  "^miniaudio" "^sentry" "^crashpad" "^lz4"
  "^SFML/" "^boost/")
