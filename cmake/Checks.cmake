# Contract checks: the link graph (at configure time) and the source scans and
# linters (as CTest tests).
#
# Labels:
#   checks  include and banned-API scans, link-graph and planted fixtures
#   lint    clang-format, cpplint and clang-tidy (docs/style.md)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

function(_se_real_target out name)
  set(_n "${name}")
  if(TARGET "${_n}")
    get_target_property(_aliased "${_n}" ALIASED_TARGET)
    if(_aliased)
      set(_n "${_aliased}")
    endif()
  endif()
  set(${out} "${_n}" PARENT_SCOPE)
endfunction()

# Collects the targets <tgt> links directly (aliases resolved; generator
# expressions other than $<LINK_ONLY:...> and link flags skipped) into <out>.
function(_se_direct_deps out tgt)
  _se_real_target(_t "${tgt}")
  get_target_property(_type "${_t}" TYPE)
  set(_deps "")
  if(NOT _type STREQUAL "INTERFACE_LIBRARY")
    get_target_property(_l "${_t}" LINK_LIBRARIES)
    if(_l)
      list(APPEND _deps ${_l})
    endif()
  endif()
  get_target_property(_i "${_t}" INTERFACE_LINK_LIBRARIES)
  if(_i)
    list(APPEND _deps ${_i})
  endif()
  set(_result "")
  foreach(_d ${_deps})
    string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" _d "${_d}")
    if(_d MATCHES "^\\$<" OR _d MATCHES "^-")
      continue()
    endif()
    _se_real_target(_d "${_d}")
    list(APPEND _result "${_d}")
  endforeach()
  list(REMOVE_DUPLICATES _result)
  set(${out} "${_result}" PARENT_SCOPE)
endfunction()

# Collects the transitive link closure of <tgt> into <out>.
function(_se_link_closure out tgt)
  set(_seen "")
  set(_stack "${tgt}")
  while(_stack)
    list(POP_BACK _stack _cur)
    _se_real_target(_cur "${_cur}")
    if(_cur IN_LIST _seen OR NOT TARGET "${_cur}")
      continue()
    endif()
    list(APPEND _seen "${_cur}")
    get_target_property(_type "${_cur}" TYPE)
    set(_deps "")
    if(NOT _type STREQUAL "INTERFACE_LIBRARY")
      get_target_property(_l "${_cur}" LINK_LIBRARIES)
      if(_l)
        list(APPEND _deps ${_l})
      endif()
    endif()
    get_target_property(_i "${_cur}" INTERFACE_LINK_LIBRARIES)
    if(_i)
      list(APPEND _deps ${_i})
    endif()
    foreach(_d ${_deps})
      string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" _d "${_d}")
      if(_d MATCHES "^\\$<" OR _d MATCHES "^-")
        continue()
      endif()
      list(APPEND _stack "${_d}")
    endforeach()
  endwhile()
  set(${out} "${_seen}" PARENT_SCOPE)
endfunction()

# Fails the configure when a module's closure contains a forbidden target,
# and writes se_checks.json for the source scans.
function(se_check_link_graph)
  get_property(_mods GLOBAL PROPERTY SE_MODULES)
  set(_errors "")
  foreach(_m ${_mods})
    _se_link_closure(_closure ${_m})
    get_target_property(_forbid ${_m} SE_FORBID)
    foreach(_f ${_forbid})
      _se_real_target(_rf "${_f}")
      if(_rf IN_LIST _closure)
        string(APPEND _errors "  ${_m} reaches forbidden target ${_f}\n")
      endif()
    endforeach()
    # ALLOW_ONLY: direct links within the list (plus the flag targets), and
    # nothing in the closure that the allowed targets do not already reach.
    get_target_property(_allow ${_m} SE_ALLOW_ONLY)
    if(_allow)
      set(_allowed se_warnings se_determinism)
      set(_reachable ${_m} se_warnings se_determinism)
      foreach(_a ${_allow})
        _se_real_target(_ra "${_a}")
        list(APPEND _allowed "${_ra}")
        _se_link_closure(_ac "${_ra}")
        list(APPEND _reachable ${_ac})
      endforeach()
      _se_direct_deps(_direct ${_m})
      foreach(_d ${_direct})
        if(NOT _d IN_LIST _allowed)
          string(APPEND _errors
            "  ${_m} links ${_d}, which is not in its ALLOW_ONLY list\n")
        endif()
      endforeach()
      foreach(_c ${_closure})
        if(NOT _c IN_LIST _reachable)
          string(APPEND _errors
            "  ${_m} reaches ${_c}, outside what its ALLOW_ONLY list reaches\n")
        endif()
      endforeach()
    endif()
    # Legacy and new targets must never meet.
    foreach(_legacy core_engine editor_app legacy_replay)
      if(_legacy IN_LIST _closure)
        string(APPEND _errors "  ${_m} reaches legacy target ${_legacy}\n")
      endif()
    endforeach()
  endforeach()
  if(TARGET core_engine)
    _se_link_closure(_legacy_closure core_engine)
    foreach(_m ${_mods})
      if(_m IN_LIST _legacy_closure)
        string(APPEND _errors "  core_engine reaches engine target ${_m}\n")
      endif()
    endforeach()
  endif()
  if(_errors)
    message(FATAL_ERROR "Link-graph violations:\n${_errors}")
  endif()

  get_property(_entries GLOBAL PROPERTY SE_CHECK_ENTRIES)
  list(JOIN _entries ",\n    " _entries)
  file(WRITE "${CMAKE_BINARY_DIR}/se_checks.json"
    "{\n  \"source_dir\": \"${CMAKE_SOURCE_DIR}\",\n  \"modules\": [\n    ${_entries}\n  ]\n}\n")
endfunction()

# Registers the source scans and linters as CTest tests.
function(se_register_checks)
  set(_tools "${CMAKE_SOURCE_DIR}/tools/checks")
  set(_json "${CMAKE_BINARY_DIR}/se_checks.json")
  set(_planted "${_tools}/planted")

  add_test(NAME checks.includes
    COMMAND Python3::Interpreter "${_tools}/check_includes.py" "${_json}")
  add_test(NAME checks.banned_api
    COMMAND Python3::Interpreter "${_tools}/check_banned_api.py" "${_json}")
  set_tests_properties(checks.includes checks.banned_api
    PROPERTIES LABELS checks)

  # Each planted fixture must make its checker fail.
  add_test(NAME checks.planted.include
    COMMAND Python3::Interpreter "${_tools}/check_includes.py"
            --planted "${_planted}/include")
  add_test(NAME checks.planted.banned_api
    COMMAND Python3::Interpreter "${_tools}/check_banned_api.py"
            --planted "${_planted}/banned_api")
  add_test(NAME checks.planted.throw
    COMMAND Python3::Interpreter "${_tools}/check_banned_api.py"
            --planted "${_planted}/throw")
  add_test(NAME checks.planted.catch
    COMMAND Python3::Interpreter "${_tools}/check_banned_api.py"
            --planted "${_planted}/catch")
  add_test(NAME checks.planted.link_graph
    COMMAND "${CMAKE_COMMAND}"
            -S "${CMAKE_SOURCE_DIR}/tests/checks/link_violation"
            -B "${CMAKE_BINARY_DIR}/planted_link_violation"
            -G "${CMAKE_GENERATOR}"
            -DSE_REPO_ROOT=${CMAKE_SOURCE_DIR})
  # --planted inverts a checker: it succeeds only if it finds a violation, so a
  # checker that crashes or finds nothing fails the test.
  set_tests_properties(checks.planted.include checks.planted.banned_api
    checks.planted.throw checks.planted.catch PROPERTIES LABELS checks)
  set_tests_properties(checks.planted.link_graph PROPERTIES LABELS checks
    PASS_REGULAR_EXPRESSION "Link-graph violations")
  # A module that links a target outside its ALLOW_ONLY list.
  add_test(NAME checks.planted.link_allow_only
    COMMAND "${CMAKE_COMMAND}"
            -S "${CMAKE_SOURCE_DIR}/tests/checks/link_allow_only_violation"
            -B "${CMAKE_BINARY_DIR}/planted_link_allow_only_violation"
            -G "${CMAKE_GENERATOR}"
            -DSE_REPO_ROOT=${CMAKE_SOURCE_DIR})
  set_tests_properties(checks.planted.link_allow_only PROPERTIES
    LABELS checks
    PASS_REGULAR_EXPRESSION
      "headless_render links extra_dep, which is not in its ALLOW_ONLY list"
    FAIL_REGULAR_EXPRESSION "headless_control")

  # Linters.
  find_program(SE_CLANG_FORMAT clang-format)
  find_program(SE_CLANG_TIDY clang-tidy)
  set(_lint "${CMAKE_SOURCE_DIR}/tools/lint")
  add_test(NAME lint.clang_format
    COMMAND Python3::Interpreter "${_lint}/run_lint.py" clang-format
            --tool "${SE_CLANG_FORMAT}")
  add_test(NAME lint.cpplint
    COMMAND Python3::Interpreter "${_lint}/run_lint.py" cpplint)
  add_test(NAME lint.clang_tidy
    COMMAND Python3::Interpreter "${_lint}/run_lint.py" clang-tidy
            --tool "${SE_CLANG_TIDY}" --build-dir "${CMAKE_BINARY_DIR}")
  set_tests_properties(lint.clang_format lint.cpplint lint.clang_tidy
    PROPERTIES LABELS lint)
  # Planted style violations prove each linter gate can fail.
  add_test(NAME lint.planted.clang_format
    COMMAND Python3::Interpreter "${_lint}/run_lint.py" clang-format
            --tool "${SE_CLANG_FORMAT}" --planted)
  add_test(NAME lint.planted.cpplint
    COMMAND Python3::Interpreter "${_lint}/run_lint.py" cpplint --planted)
  add_test(NAME lint.planted.clang_tidy
    COMMAND Python3::Interpreter "${_lint}/run_lint.py" clang-tidy
            --tool "${SE_CLANG_TIDY}" --build-dir "${CMAKE_BINARY_DIR}"
            --planted)
  set_tests_properties(lint.planted.clang_format lint.planted.cpplint
    lint.planted.clang_tidy PROPERTIES LABELS lint)
endfunction()
