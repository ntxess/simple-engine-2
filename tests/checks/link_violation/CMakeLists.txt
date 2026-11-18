# Planted link-graph violation: configuring this project must fail with
# "Link-graph violations" (CTest checks.planted.link_graph).

cmake_minimum_required(VERSION 3.31)
project(se_link_violation LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${SE_REPO_ROOT}/cmake")
include(Warnings)
include(SeModule)

add_library(forbidden_platform INTERFACE)
se_add_module(headless_core KIND INTERFACE
  PUBLIC_DEPS forbidden_platform
  FORBID forbidden_platform)

# Checks.cmake needs Python only for the source scans; the link check does not.
include(Checks)
se_check_link_graph()
