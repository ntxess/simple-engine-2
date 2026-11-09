# se_render, M5 part: the SDL_GPU device and the clear-only frame
# transaction (docs/milestone-specs.md, M5). M7 grows it into Renderer2D over
# se_render_core's RenderList. Main thread only; not deterministic.
# M6a: present.h, the presentation signals of the SDL patch (SE_SDL_PATCHED
# is 1 when cmake/Dependencies.cmake applied it, else every query reports
# unsupported).

se_add_module(se_render KIND STATIC
  SOURCES
    src/frame_recorder.cc
    src/gpu_device.cc
    src/present.cc
  PUBLIC_DEPS
    se_core
    se_platform
    se_render_core
    SDL3::SDL3-static
  FORBID
    se_imgui miniaudio sentry lz4_static)
if(SE_SDL_PATCHED)
  target_compile_definitions(se_render PUBLIC SE_SDL_PATCHED=1)
else()
  target_compile_definitions(se_render PUBLIC SE_SDL_PATCHED=0)
endif()
