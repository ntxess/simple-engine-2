# Dependencies

New dependencies are pinned in cmake/Dependencies.cmake by URL and SHA256
(ADR-002). Each archive is downloaded once into `.deps/<name>/` and every
build tree extracts the cached file. To update a pin: change the URL and hash
together, delete the old archive from `.deps/`, record the change here.

| Name | Pin | Archive (bytes) | SHA256 | License | First used | Why |
|---|---|---|---|---|---|---|
| SDL3 | release-3.4.20 | SDL3-3.4.20.tar.gz (15,656,494) | `f3bb06f5…3ef8581f` | Zlib | M5 | Window, input, SDL_GPU (D3D12/Vulkan), main callbacks, filesystem |
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

Windows system libraries (from the SDK, no pin): comctl32 (the window
subclass, M5) and dxgi (the vblank sampler and the display outputs, M6b),
both linked by se_platform only.

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
