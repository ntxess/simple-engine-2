# asset_cook: cooks the textures of config/texture.json into .seatlas pages
# (docs/milestone-specs.md M9; se_assets/atlas_page.h). Tool code (TEST:
# exempt from the banned-API scan). The cook runs at build time into
# <build>/bin/cooked, next to the executables, where the runtime looks for
# pages by default (--cooked-root overrides it):
#
#   se_cooked_textures   cooks config/texture.json; rampage and the tests
#                        that run it depend on it
#
# The table is read at configure time for the PNGs the cook depends on;
# editing the table re-runs the configure.

se_add_module(asset_cook KIND EXECUTABLE
  SOURCES
    main.cc
  PRIVATE_DEPS
    se_assets
    se_core
    se_platform
    se_render
  TEST)

set(SE_COOKED_DIR "${CMAKE_BINARY_DIR}/bin/cooked" CACHE INTERNAL
    "Where the build cooks atlas pages")
set(_se_table "${CMAKE_SOURCE_DIR}/config/texture.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_se_table}")
file(READ "${_se_table}" _se_table_json)
string(JSON _se_count LENGTH "${_se_table_json}")
set(_se_sources)
if(_se_count GREATER 0)
  math(EXPR _se_last "${_se_count} - 1")
  foreach(_se_i RANGE ${_se_last})
    string(JSON _se_name MEMBER "${_se_table_json}" ${_se_i})
    string(JSON _se_png GET "${_se_table_json}" "${_se_name}")
    list(APPEND _se_sources "${CMAKE_SOURCE_DIR}/${_se_png}")
  endforeach()
endif()
set(_se_stamp "${SE_COOKED_DIR}/texture.stamp")
add_custom_command(
  OUTPUT "${_se_stamp}"
  COMMAND asset_cook --content-root "${CMAKE_SOURCE_DIR}"
          --table config/texture.json --out "${SE_COOKED_DIR}"
          --stamp "${_se_stamp}"
  DEPENDS asset_cook "${_se_table}" ${_se_sources}
  COMMENT "asset_cook config/texture.json"
  VERBATIM)
add_custom_target(se_cooked_textures ALL DEPENDS "${_se_stamp}")

if(SE_BUILD_TESTS)
  # A cook into a scratch directory: every page cooks, then a second run
  # finds them fresh; a missing table fails.
  set(_se_cook_out "${CMAKE_BINARY_DIR}/asset_cook_out")
  add_test(NAME asset_cook.cook
    COMMAND asset_cook --content-root "${CMAKE_SOURCE_DIR}"
            --table config/texture.json --out "${_se_cook_out}" --force)
  add_test(NAME asset_cook.fresh
    COMMAND asset_cook --content-root "${CMAKE_SOURCE_DIR}"
            --table config/texture.json --out "${_se_cook_out}")
  set_tests_properties(asset_cook.cook PROPERTIES
    LABELS "unit;tools" FIXTURES_SETUP se_cook
    PASS_REGULAR_EXPRESSION
      "${_se_count} textures: ${_se_count} cooked, 0 fresh, 0 failed")
  set_tests_properties(asset_cook.fresh PROPERTIES
    LABELS "unit;tools" FIXTURES_REQUIRED se_cook
    PASS_REGULAR_EXPRESSION
      "${_se_count} textures: 0 cooked, ${_se_count} fresh, 0 failed")
  add_test(NAME asset_cook.bad_table
    COMMAND asset_cook --content-root "${CMAKE_SOURCE_DIR}"
            --table no/such/table.json --out "${_se_cook_out}")
  set_tests_properties(asset_cook.bad_table PROPERTIES
    LABELS "unit;tools" WILL_FAIL TRUE)
endif()
