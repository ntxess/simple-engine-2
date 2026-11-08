# Planted ALLOW_ONLY violation: a module allowed to link only core_dep also
# links extra_dep, so configuring this project must fail with "headless_render
# links extra_dep, which is not in its ALLOW_ONLY list" (CTest
# checks.planted.link_allow_only). The control module stays inside its list
# and must not be reported.

cmake_minimum_required(VERSION 3.31)
project(se_link_allow_only_violation LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${SE_REPO_ROOT}/cmake")
include(Warnings)
include(SeModule)

add_library(core_dep INTERFACE)
add_library(extra_dep INTERFACE)
se_add_module(headless_render KIND INTERFACE
  PUBLIC_DEPS core_dep extra_dep
  ALLOW_ONLY core_dep)
se_add_module(headless_control KIND INTERFACE
  PUBLIC_DEPS core_dep
  ALLOW_ONLY core_dep)

# Checks.cmake needs Python only for the source scans; the link check does not.
include(Checks)
se_check_link_graph()
