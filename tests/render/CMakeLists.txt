# RenderList tests (label render): interpolation, culling, sorting, batching,
# quads and the no-allocation contract of se_render_core
# (docs/milestone-specs.md, M3c).
#
# se_render_tests replaces the global operator new to count allocations
# (alloc_counter.cc), so it is its own executable. Sanitizer builds keep
# their runtime's allocator and check buffers only.

if(SE_SANITIZER STREQUAL "")
  set(_se_count_allocations 1)
else()
  set(_se_count_allocations 0)
endif()

se_add_module(se_render_tests KIND EXECUTABLE
  SOURCES
    alloc_counter.cc
    culling_test.cc
    interpolation_test.cc
    no_alloc_test.cc
    quad_test.cc
    render_components_test.cc
    render_fixture.cc
    sort_batch_test.cc
  PRIVATE_DEPS
    se_core
    se_render_core
    se_test_support
    Catch2::Catch2WithMain
  DETERMINISTIC
  TEST)
target_compile_definitions(se_render_tests PRIVATE
  SE_COUNT_ALLOCATIONS=${_se_count_allocations})

catch_discover_tests(se_render_tests
  DISCOVERY_MODE PRE_TEST
  ADD_TAGS_AS_LABELS
  TEST_PREFIX "render."
  PROPERTIES LABELS render)
