# se_audio: the audio engine on miniaudio 0.11.26 (docs/milestone-specs.md,
# M8; ADR-076). Main owns the AudioEngine (ma_engine) and is its only
# caller; miniaudio mixes on its own device thread. Groups Music and
# Effects, refcounted PCM, voices with scheduled starts, the end-of-sound
# SPSC ring and the per-frame sweep, the null-backend fallback and a
# no-device mode for tests. Not deterministic. miniaudio is confined here:
# every other engine module FORBIDs it, and tools/checks/check_includes.py
# rejects <miniaudio.h> anywhere else (checks.planted.miniaudio_include).

se_use_deps(miniaudio)
# miniaudio is third-party C: its headers are system headers for us (no
# warnings, no clang-tidy findings from them).
set_property(TARGET miniaudio PROPERTY SYSTEM TRUE)

se_add_module(se_audio KIND STATIC
  SOURCES
    src/audio_engine.cc
    src/pcm.cc
    src/pcm_clock.cc
  PUBLIC_DEPS
    se_core
  PRIVATE_DEPS
    miniaudio
  ALLOW_ONLY
    se_core miniaudio
  FORBID
    SDL3::SDL3-static se_imgui sentry lz4_static)
