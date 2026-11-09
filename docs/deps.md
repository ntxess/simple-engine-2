# Dependencies

New dependencies are pinned in cmake/Dependencies.cmake by URL and SHA256
(ADR-002). Each archive is downloaded once into `.deps/<name>/` and every
build tree extracts the cached file. To update a pin: change the URL and hash
together, delete the old archive from `.deps/`, record the change here.

| Name | Pin | Archive (bytes) | SHA256 | License | First used | Why |
|---|---|---|---|---|---|---|
| SDL3 | release-3.4.20 | SDL3-3.4.20.tar.gz (15,656,494) | `f3bb06f5…3ef8581f` | Zlib | M5 | Window, input, SDL_GPU (D3D12/Vulkan), main callbacks, filesystem; carries the presentation patch below from M6 (`SE_SDL_PATCH`, off in msvc-unpatched-dev) |
| Catch2 | v3.16.1 | v3.16.1.tar.gz (1,228,880) | `d16bd1b8…3f28d777d` | BSL-1.0 | M0 | Tests under CTest (3.16.1 fixes MSVC optimisation after benchmark headers) |
| spdlog | v1.17.0 | v1.17.0.tar.gz (281,801) | `d8862955…cd3a744` | MIT | M2 | Log sinks, used synchronously by the log worker (design dependency list) |
| glaze | v9.1.0 | v9.1.0.tar.gz (2,378,235) | `55f8a675…dbff4bd4` | MIT | M2 | Reflection-based JSON and BEVE for the component registry |
| Tracy | v0.14.1 | v0.14.1.tar.gz (6,173,646) | `bf4af567…b251c1` | BSD-3 | M4 | On-demand profiling; the profiler GUI must be v0.14.x |
| sentry-native | 0.17.6 | sentry-native.zip (9,114,248) | `40b4b70c…d2b5055` | MIT (crashpad: Apache-2.0) | M4 | Crashpad out-of-process crash and hang dumps; transport off, nothing uploaded |
| miniaudio | 0.11.26 | 0.11.26.tar.gz (4,572,837) | `838ba55a…969c600` | Public domain / MIT-0 | M8 | Audio engine owned by the main thread |
| LZ4 | v1.10.0 | lz4-1.10.0.tar.gz (387,114) | `53751290…d792ec5f48b` | BSD-2 | M9 | Atlas page compression decoded in ~2 ms lane steps |
| Dear ImGui | v1.93.0-docking | v1.93.0-docking.tar.gz (2,365,310) | `746c2655…1496f1a5` | MIT | M10 | Editor UI with the first-party SDL3 and SDL_GPU backends |
| stb | (pinned at M7) | | | MIT / public domain | M7 | PNG decoding at import |
| EnTT | 3.16.0 (vendored in dep/) | | | MIT | M2 | ECS; built with `ENTT_USE_ATOMIC` everywhere |

Full hashes are in cmake/Dependencies.cmake.

## SDL presentation patch (M6a)

`cmake/patches/sdl3-3.4.20-se-presentation.patch` (96,195 bytes, 2,372
lines; the build checks its SHA256) is a `git diff` against the extracted
SDL3-3.4.20.tar.gz, applied by cmake/ApplyPatch.cmake when `SE_SDL_PATCH`
is on; `.gitattributes` keeps it byte-exact. It adds the `SDL_SE_*` API of
`include/SDL3/SDL_gpu_se.h` (ADR-050 to ADR-053; the engine side is
engine/se_render/include/se_render/present.h).

| File | Added | Removed | What |
|---|---|---|---|
| include/SDL3/SDL_gpu_se.h | 260 | 0 | new: the public API |
| src/gpu/SDL_gpu_se.c | 214 | 0 | new: entry points, dispatch by backend name, QPC to SDL ticks |
| src/gpu/SDL_gpu_se_c.h | 66 | 0 | new: the backend table |
| src/gpu/d3d12/SDL_gpu_d3d12_se.h | 96 | 0 | new: D3D12 state and hook prototypes |
| src/gpu/d3d12/SDL_gpu_d3d12_se_impl.h | 574 | 0 | new: D3D12 hooks and backend |
| src/gpu/d3d12/SDL_gpu_d3d12.c | 20 | 1 | 2 includes, 3 `se` members, hooks in DestroyDevice, ResizeSwapchain (flags), DestroySwapchain, CreateSwapchain (flags, created), AcquireCommandBuffer, Submit (before Close, after the fence), Cancel |
| src/gpu/vulkan/SDL_gpu_vulkan_se.h | 151 | 0 | new: Vulkan state and hook prototypes |
| src/gpu/vulkan/SDL_gpu_vulkan_se_impl.h | 692 | 0 | new: Vulkan hooks and backend |
| src/gpu/vulkan/SDL_gpu_vulkan.c | 30 | 1 | 2 includes, 3 `se` members, CHECK_VULKAN_ERROR_AND_RETURN (device lost), DestroyDevice, CreateSwapchain (end), AcquireCommandBuffer, Submit (present chain, before End, after the fence), Cancel, CreateLogicalDevice (extensions, retry) |

Every line it touches in SDL's own files carries the comment
`simple-engine-2 presentation patch` (`simple-engine-2 patch` inside the
error macro). To rebase onto a later SDL:

1. Extract the new tarball outside the repository, `git init`, commit it
   as is, and copy the seven new files over.
2. Re-apply the hook lines by name (each sits next to the SDL statement
   the table names; `git apply --3way` against the old patch does most of
   it). Check that D3D12 still creates every swapchain in
   D3D12_INTERNAL_CreateSwapchain and resizes it only in
   D3D12_INTERNAL_ResizeSwapchain, and that Vulkan still presents in one
   place in VULKAN_Submit.
3. `git add -A && git diff --cached <pristine commit>`, with the
   description lines on top, into a patch named for the new version; update
   the file name in cmake/Dependencies.cmake and this section.
4. Run `scripts/ci.ps1`, `-Desktop`, `-Presets msvc-unpatched-dev` and the
   WSL presets; delete `_deps/sdl3-*` and `CMakeFiles/fc-*/sdl3` in old build
   trees (ApplyPatch refuses a tree a different patch was applied to).

The new functions are not in SDL's dynamic API tables (static linking
only). On Windows the files the patch touches are written with CRLF line
endings by `git apply` (git's core.autocrlf), the rest of SDL keeps LF;
both compile. Upstreaming needs the owner: SDL's AGENTS.md and CLAUDE.md
(in the tarball) refuse contributions made with generative AI, which this
patch is (docs/gates.md).

## Measurements (2026-11-08, Ryzen 7 7700X, MSVC 14.51 Dev, Ninja)

- deps-smoke: every pinned dependency builds (665 steps in 29 s), including
  SDL3 static, sentry with crashpad and crashpad_handler, TracyClient,
  miniaudio, lz4_static and spdlog.
- glaze: a translation unit reflecting 20 aggregates to JSON and BEVE compiles
  in about 6 s, so glaze is confined to the registry translation units.
- Dear ImGui v1.93.0-docking contains imgui_impl_sdl3 and imgui_impl_sdlgpu3.

## Not adopted

enkiTS (first-party ForkJoinPool instead), EnTT 4.0 (breaking; revisit after
M3), nlohmann/json (fallback only if glaze compile cost becomes a problem),
Boost and toml++ (legacy only).
