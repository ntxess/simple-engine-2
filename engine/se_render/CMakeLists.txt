# se_render, M5 part: the SDL_GPU device and the clear-only frame
# transaction (docs/milestone-specs.md, M5). M7 grows it into Renderer2D over
# se_render_core's RenderList. Main thread only; not deterministic.

se_add_module(se_render KIND STATIC
  SOURCES
    src/frame_recorder.cc
    src/gpu_device.cc
  PUBLIC_DEPS
    se_core
    se_platform
    se_render_core
    SDL3::SDL3-static
  FORBID
    se_imgui miniaudio sentry lz4_static)
