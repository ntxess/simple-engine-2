# se_assets: the background lane and the AssetRegistry (docs/milestone-specs.md
# M9; docs/design/mt-design.md "Threads", "Audio And Assets"):
#   background_lane.h  one thread (role kBackground): SPSC request and
#                      completion rings, a binary_semaphore with the 500 ms
#                      hot-reload scan as its timeout, Urgent and Background
#                      classes with per-step QoS, bounded steps, strands,
#                      revisions and Durable jobs
#   lane_jobs.h        reads, SaveRequests (temporary file and rename, the
#                      sharing-violation retry), flushes, watches, prefabs
#                      and scene chunks (glaze, payloads_glaze.cc only)
#   atlas_page.h       cooked .seatlas pages: LZ4 blocks of about 256 KB,
#                      decoded one block per lane step; stb_image at import
#                      (tools/asset_cook, and the lane's re-cook in dev
#                      builds)
#   sound_decoder.h    the sound-loading entry point M8's se_audio fills
#   asset_registry.h   the typed stores, Handle<T>, AssetScopes and hot
#                      reload, on main; the GPU through TextureUploadPort
# Not deterministic. se::fs (se_platform) is its only path to SDL: the
# module includes no SDL, GPU, window, audio or Win32 header (miniaudio
# stays in se_audio).

se_use_deps(lz4 glaze stb)

se_add_module(se_assets KIND STATIC
  SOURCES
    src/asset_registry.cc
    src/atlas_page.cc
    src/background_lane.cc
    src/cook.cc
    src/lane_fault_boundary.cc
    src/lane_jobs.cc
    src/payloads_glaze.cc
    src/sound_job.cc
  PUBLIC_DEPS
    se_core
    se_os
    se_platform
  PRIVATE_DEPS
    lz4_static
    glaze::glaze
    se::stb
  FORBID
    se_render se_runtime se_imgui miniaudio sentry
  BANNED_INCLUDES
    "^SDL3?/" "^imgui" "^backends/imgui" "^windows\.h$" "^Windows\.h$"
    "^miniaudio" "^sentry" "^crashpad" "^SFML/" "^boost/" "^se_render/"
    "^se_runtime/")
# Internal headers are included as "src/<name>.h".
target_include_directories(se_assets PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
