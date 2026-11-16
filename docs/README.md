# simple-engine-2 documentation

The engine is being redesigned on the `redesign/engine-v2` branch. The new
targets grow beside the legacy SFML tree, which is kept as a deterministic
behaviour reference until every rule and editor operation is ported.

| Document | What it holds |
|---|---|
| [design/mt-design.md](design/mt-design.md) | The specification being implemented: one owner thread with fork-join workers and helper threads, the 25 required changes, API sketch, build order |
| [design/dedicated-driver.md](design/dedicated-driver.md) | The dedicated 120 Hz sim-thread driver, built behind `--sim-driver=threaded` |
| [design/costfree-end-state.md](design/costfree-end-state.md) | The end state the design builds on: modules, World, renderer, editor, assets |
| [design/platform-constraints.md](design/platform-constraints.md) | Threading rules of SDL3, Dear ImGui, EnTT, miniaudio and Windows |
| [design/prototype-measurements.md](design/prototype-measurements.md) | The measurement rig's results on the 7700X |
| [decisions.md](decisions.md) | Architecture decision records |
| [style.md](style.md) | Google C++ style, linting and the exceptions policy |
| [deps.md](deps.md) | Pinned dependencies, hashes and why each is used |
| [gates.md](gates.md) | Verification gates that need hardware or setup not on the dev machine |
| [ledger.md](ledger.md) | Completeness ledger: every legacy feature ported, re-specified or deleted |
| [implementation-plan.md](implementation-plan.md) | The approved milestone plan (M0-M19) |
| [milestone-specs.md](milestone-specs.md) | Detailed specs for M1 to M10, M12, M14 and M17a |
| [rules/gameplay-rules.md](rules/gameplay-rules.md) | Declared gameplay rules R1-R13 applied to the legacy reference (milestones M1b and M3a) |
| [../tests/fixtures/reference/README.md](../tests/fixtures/reference/README.md) | The legacy reference scenarios, inputs and golden projections (legacy_replay, tests/legacy, ref.sandbox) |
| [../bench/thresholds.json](../bench/thresholds.json) | se_bench gates and the measurements behind them |

## Modules and tools

| Target | Directory | What it is |
|---|---|---|
| se_core | engine/se_core | Kernel: World, Schedule, component registry, hashing, detmath, jobs, time (M2); the input placement rule (edge_placement.h: PlaceEdge, the Locked and Free tick windows the stepper uses) and the input buffer window (input_buffer.h) (M8, ADR-073); the PCM type the lane and the audio engine share (pcm.h, ADR-087); the dedicated driver's channels: SpscQueue, Doorbell, FaultBox, the Snapshot slot and the SnapshotChannel, and SimOwnerScope (M17a, ADR-083, ADR-085); the threaded ForkJoinPool (jobs.h: sites kept by the pool, per-worker wake words, batch claims, ChunkOutputs, the fork fault boundary, ForkStats with join tails, the 4x-median fallback, per-efficiency-class calibration, the spin, chaos mode; the built-in PrevTransformCopy is a fork site) (M14, ADR-107 to ADR-109) |
| se_os | engine/se_os | The headless OS layer for threads: StartThread and RoleScope (names, roles, registry slots, QoS, CPU sets), the CPU topology and pool sizes, the unbiased clock, debugger detection, ExecutableDir; depends on se_core only (M4, ADR-029); the sim thread's PreciseTimer (precise_wait.h, M17a, ADR-084); ForkWorkers, the fork pool's worker threads (fork_workers.h: role kForkWorker, Qos::kHigh, placement by efficiency class, BatteryPowered, ChaosSeedFromEnv) and ReadSystemCpuTimes (process.h) (M14, ADR-107, ADR-108) |
| se_log | engine/se_log | The log worker: drains the per-thread log rings into the spdlog file, stderr and panel sinks; the only module that includes spdlog (M4, ADR-032) |
| se_sim | engine/se_sim | Game modules, GameStateStack, SessionLog, replays, SimDriver (M2); AudioEvents and the AudioSink contract (M8); the TickGate (M9); the dedicated sim driver, headless: SimThread (role kSim; sessions built and destroyed on it, M17a's commands, the tick as a fault boundary), PacedClock (absolute deadlines, the timer-plus-spin wait with a capped margin, phi, catch-up and dilation; ManualClock for tests) and the InputFolder (M8's half-open placement, holder counts) (M17a, ADR-083, ADR-084, ADR-086); GameStateStack::Revision, which the snapshot generation follows (ADR-086); the App keeps the inline driver until M17b |
| se_audio | engine/se_audio | The audio engine on miniaudio (the only module that links or includes it): one ma_engine owned by main, Music and Effects groups with the settings file's volumes, the null-backend fallback and a no-device mode for tests; refcounted PCM (WAV and FLAC through miniaudio's decoder), 256 fixed voices with scheduled starts, per-group caps that steal the oldest, the end-of-sound SPSC ring and the per-frame sweep, scopes, reloads for new voices only; the PcmClock (M8, ADR-076) |
| se_render_core | engine/se_render_core | Headless renderer half: SpriteRenderer, Camera2D, BuildRenderList; depends on se_core only (M3); both ends of the dedicated driver's snapshots: AppendRenderView (a World into a slot's render part, on the sim thread) and SnapshotRenderSource (slots into RenderLists: the newest 3, tau and alpha, per-entity interpolation, starvation, hard cuts), sharing BuildRenderList's quad code (M17a, ADR-086) |
| se_crash | engine/se_crash | Out-of-process crash capture on crashpad: Start/CrashGuard, RegisterStream, DumpWithoutCrash, the terminate annotation; nothing uploaded; a stub on Linux and in msvc-asan (M4, ADR-033, ADR-034). The watchdog skeleton: role kWatchdog, the HangDetector (boot budget; hangs declared from M12), RequestDump, the shutdown deadline (M4 integration, ADR-043) |
| se_assets | engine/se_assets | The background lane (role kBackground: SPSC request and completion rings, a binary_semaphore whose 500 ms timeout drives the hot-reload scan, Urgent and Background classes with per-step QoS, bounded steps, strands, revisions, Durable jobs, the sharing-violation retry, a fault boundary per step) and its jobs (reads, SaveRequests and flushes, prefabs and scene chunks through glaze, sounds through M8's decoder, cooked `.seatlas` atlas pages one LZ4 block per step); the AssetRegistry on main (typed stores, Handle<T>, AssetScopes, hot reload, GpuResources behind an upload port) (M9, ADR-078 to ADR-082) |
| game_rampage | games/rampage | The legacy Sandbox's gameplay on the kernel: components, systems, scenes and prefabs (assets/), legacy scenarios, projections (M3) |
| se_platform | engine/se_platform | SDL3 on the main thread: SDL's initialisation, the event filter and Inbox, WakeMain, the window and its Win32 subclass (Modal, session end, power), se::fs on SDL's filesystem API (M9: stepwise FileReader with FILE_SHARE_DELETE on Windows, FileWriter, TryReplace with POSIX semantics and sharing violations as kBusy, the fault and delay injector in test builds; ADR-080), fault dialogs off under SE_NO_MESSAGE_BOX=1 and in every test executable (SDL's assertion dialog included: a failed SDL assertion prints and aborts, ADR-070) (M5); the input sampler (role kInputSampler: SDL_UpdateGamepads at 1000 Hz on AC and 250 Hz on battery, parked on a doorbell while Suspended or Idle, inside ADR-047's window) and the open gamepads (M8, ADR-075); the display outputs (QueryDisplayConfig's current mode of the window's monitor, ADR-055) and the vblank sampler (role kVblankSampler: WaitForVBlank on the window's DXGI output, stamps in the SDL_GetTicksNS domain, a median period; a stub elsewhere) (M6b, ADR-056) |
| se_render | engine/se_render | The SDL_GPU device (D3D12 on Windows, Vulkan on Linux) (M5); Renderer2D on main: the frame transaction (FrameRecorder, its fault boundary and the --inject-frame-fault hook), GpuResources (texture handles with a magenta fallback, the retire queue, the 8 MB upload queue, device-loss release and recreation), the sprite and blit pipelines, the composite and ViewportTargets, the texture table and PNGs through stb; HLSL shaders in shaders/, SPIR-V committed in shaders/generated/, DXIL built with the SDK's dxc (M7, ADR-059 to ADR-063); `se::present` (present.h): the SDL presentation patch's frame-latency wait, present statistics, GPU timestamps, native handles, device status and present mode, all unsupported in the unpatched build (M6a, ADR-050 to ADR-053) |
| se_runtime | engine/se_runtime | The App on SDL's main callbacks: boot steps, the frame, the lifecycle and two-phase quit state machines, BasicInputService, the noexcept callbacks (`SE_DEFINE_SDL_APP`) (M5); the main thread's RoleScope, crash capture and its dump streams, the log worker and the watchdog in boot order (integration.h; ADR-042); the InputService (stamped edges latched per tick by the placement rule, held state, axes, focus-loss clears with source `cleared`) and the ActionMap (the game's bindings and the settings file's key-bindings as SDL keycodes, default pad bindings, unknown names a boot error), the settings file reader (user_settings.h), `--record` session replays and `--no-raw-keyboard` (M8, ADR-073, ADR-074), replacing BasicInputService; the AudioBridge (each World's audio events started at max(now, tick anchor + D), D = B + T/2 from the plan and the pacer, late and cosmetic rules, a voice scope per World), the audio engine at boot step 10 with the sound table's sounds and `--audio device|null|none` / SE_AUDIO (M8, ADR-076, ADR-077); the FramePacer over the pure PacerPolicy (Paced, Standard, VRR, HalfRate, adaptive latency, misses from present statistics or the latency wait; `--pacing`) and the PresentPort it presents through (M6b, ADR-054, ADR-057, ADR-058); RenderPresentPort, the adapter over `se::present` and Renderer2D, misses from the frame-latency wait on the vblank grid whenever it can resolve a present, with present statistics counted beside it (and checked on their own after DWM's composition offset, ADR-072) and the source only without a waitable (ADR-066), HalfRate that backs off instead of flapping (ADR-071), the GPU timings as FrameCosts' g, and device loss (DeviceLost, recreation on main, `--inject-device-lost`) (M6/M7 integration, ADR-066 to ADR-068); the background lane and the AssetRegistry in the boot and shutdown order (asset_runtime.h: an AssetScope per GameState, the hold until it is resident through SimDriver's TickGate, GpuResources behind the registry's upload port, the lane's QuitProvider for Durable jobs; `--cooked-root`, `--hot-reload`, `--lane-step-trace`) (M9, ADR-081) |
| rampage | games/rampage/app | The rampage executable: the sandbox scene at 120 Hz in a window (M5), drawn textured by Renderer2D (M7) from the cooked pages of config/texture.json loaded through the background lane, hot-reloading a changed texture (M9); crashpad_handler is copied next to it |
| asset_cook | tools/asset_cook | Cooks the textures of config/texture.json into `.seatlas` pages (a header and independent LZ4 blocks of about 256 KB of RGBA8), one per texture; CMake runs it at build time into `<build>/bin/cooked` (M9, ADR-078) |
| input_probe | tools/input_probe | A window that logs each real key event's SDL stamp (SDL's raw-input thread with `--raw-keyboard on`, the Win32 message time with `off`) and its poll delay, and reports the stamps' resolution; needs a person (docs/gates.md "Win32 key stamp resolution"); only `--help` runs unattended (`tools.input_probe.usage`) (M8) |
| make_sounds | tools/audio/make_sounds.py | Writes rampage's sounds (assets/sounds/hit.wav, death.wav) by integer synthesis; `--check` is `checks.sounds_fresh` (Python 3, stdlib only) (M8) |
| se_audio_tests | tests/audio | se_audio and the AudioBridge mixed offline or on the null backend (`audio.*`, label unit, TSan and ASan in WSL): 8.33 ms between two ticks' sounds by onset detection, late and cosmetic rules, voice-cap stealing, the end ring and the sweep, stop, scope release and reload while a mixer thread plays, the fallback; the real device at volume 0 (label desktop) |
| se_replay | tools/se_replay | Headless runs of a game module: per-tick hashes, replays, projections; `--edges-out` lists a replay's recorded edges with their sources (M8) |
| render_shot | tools/render_shot | A rampage scenario at one tick rendered offscreen by se_render into a PNG, and the image comparison (`--compare`, the golden tolerance: the spec's three conditions, 90% of the drawn pixels, every sprite-sized object at least a quarter within, no solid block drawn in one image only, no fallback magenta; ADR-065, ADR-069); drives the legacy image comparison (whose legacy image must equal tests/golden/render/legacy/) and the rampage fault-injection tests, `render.fault.<driver>.device_lost` included (M7, ADR-065; ADR-068) |
| build_shaders | tools/shaders/build_shaders.py | Compiles the renderer's HLSL: SPIR-V headers through glslang (committed; `--check` is the WSL freshness test), DXIL headers through dxc at build time (Python 3, stdlib only) |
| compare_images | tools/render_shot/compare_images.py | Compares two PNGs with the renderer's tolerance (the same rule as render_shot, ADR-069) and writes a diff image (Python 3, stdlib only) |
| refcmp | tools/refcmp/refcmp.py | Compares two `.seproj` projections within the M3 tolerances (Python 3, stdlib only) |
| read_dump | tools/minidump/read_dump.py | Decodes a crash dump: threads, exception, annotations and the first-party streams (breadcrumbs, log rings, InputFrame ring), and lists every record that does not fit its index (a stale or torn InputFrame slot, log sequence numbers out of order) under "problems", apart from the one a running writer was overwriting (ADR-046); the crash tests' oracle (Python 3, stdlib only) |
| presentmon | tools/presentmon/analyze.py, run.ps1 | Summarises a PresentMon CSV for one process: displayed latency p50/p99, dropped and repeated frames, present intervals, with optional limits (Python 3, stdlib only; `presentmon.analyze` tests it); run.ps1 captures rampage with PresentMon (not installed; docs/gates.md) (M6b) |
| se_pacing_tests | tests/pacing | PacerPolicy, the FramePacer on a fake clock and present port, the vblank sampler on a fake source (label unit, TSan included); desktop: the sampler on this machine's outputs, SDL's refresh against the OS's, Standard without the patch, the VRR deadline, Paced's misses over 20 s on D3D12 and on Vulkan with the source they came from, and an injected late frame draining (`pacing.*`) |
| se_crash_probe | tests/crash | Registers dump streams with known contents, then crashes or requests dumps; the `crash.*` tests (label crash) check its dumps with read_dump. `crash.rampage_*` (labels desktop and crash) check the M4 exit dumps of the rampage executable |
| se_present_tests | tests/present | The present API on both GPU drivers (`present.d3d12.*`, `present.vulkan.*`, chosen with SE_GPU_DRIVER): caps, GPU timing, handles and status (label gpu), and the frame-latency waits, statistics, resize and present-mode changes (label desktop) |
| SDL patch | cmake/patches | sdl3-3.4.20-se-presentation.patch: `include/SDL3/SDL_gpu_se.h` and its D3D12 and Vulkan backends, applied with `SE_SDL_PATCH` (docs/deps.md has the rebase notes) |
| se_bench | bench/se_bench | Benchmark suites and their Release gates (`bench.*`); M14's fork overhead and join tails at W = 1/3/7 (`fork_empty_w*` back to back, `fork_parked_w*` waking parked workers, `fork_50us_w*` with 50 us chunks) |
| se_jobs_tests | tests/jobs | The threaded ForkJoinPool (`jobs.*`, label unit, TSan in WSL): exactly-once chunks, visibility, no lost wake, exceptions, stop, nested forks, per-chunk outputs, PerformanceOnly, ForkStats, the fallback window, the spin, calibration, for W = 1/3/7 and eight chaos seeds; identical per-tick hashes for W = 0/1/3/7 x 8 seeds on the kernel and rampage goldens and scenarios, pile-up scenes and the RenderList (M14, ADR-109) |
| se_wake_soak | tests/jobs | The fork pool's wake-stall soak: every wake's latency with its stamps and the machine's load per 100 ms; `jobs.wake_soak` (labels slow and soak, Release only, 10 minutes) fails on a wake over 2 ms while the machine had idle cores, `--strict` on any; `jobs.wake_soak_smoke` runs 2 s everywhere (M14, ADR-110) |
| se_lane_bench | bench/lane_steps | `bench.lane_steps`: the background lane's steps (a 1 MB read, a 256 KB LZ4 block decode, a scene chunk parse), each gated on its p99 (M9) |
| legacy tree | core/, app/ | The SFML reference (`SE_BUILD_LEGACY`, legacy presets) |

## Threads

Every engine thread is a named role started through `se::StartThread`
(se_os), with a ThreadRegistry slot that carries its breadcrumb and log
ring into crash dumps; `integration::EngineThreadsRunning()` counts the
live ones besides main. The App starts and stops them in this order
(docs/design/mt-design.md "Shutdown Order"; ADR-047, ADR-075, ADR-088), and
logs the teardown as "shutdown order: ...":

| Thread (OS name) | Role | Module | Started | Stopped | What it does |
|---|---|---|---|---|---|
| main | kMain | se_runtime | SDL's main thread, from SDL_AppInit | the end of SDL_AppQuit | window, events, GPU, every World in inline mode, the audio engine's calls, the AssetRegistry |
| log-worker | kLogWorker | se_log | boot step 2 | shutdown step 16 | drains the per-thread log rings into the file, stderr and the panel ring |
| vblank-sampler | kVblankSampler | se_platform | boot step 9 (Windows, with a window) | shutdown step 9 (or detached in its wait) | stamps the window's output's vblanks for the FramePacer |
| watchdog | kWatchdog | se_crash | boot step 9 | shutdown step 17 (the last) | the boot budget, hangs, requested dumps, the 5 s shutdown deadline |
| bg-lane | kBackground | se_assets | boot step 9, after the watchdog | shutdown step 8 (Durable jobs finish) | the background lane: reads, cooked pages, sound decoding, saves, the hot-reload scan; calls WakeMain only inside ADR-047's window |
| input-sampler | kInputSampler | se_platform | the first iterate, after the wake scope opens (ADR-047) | before shutdown step 4 | SDL_UpdateGamepads at 1 kHz (250 Hz on battery), parked while Suspended or Idle |
| sim | kSim | se_sim | `SimThread::Start` (tests; the App wires it in M17b) | Quit, `SimThread::Stop` | the dedicated sim driver: owns its session's Worlds and runs SimDriver::RunTick on PacedClock deadlines (M17a) |
| fork-worker-<i> | kForkWorker | se_core's ForkJoinPool, started by se_os ForkWorkers | when a site of the pool is first enabled (none is in a shipped build, so none starts) | the pool's destructor (with its session) | deterministic fork-join chunks, at most W_max = ForkPoolSize (M14, ADR-107) |

Library threads (role kLibrary, no slot): miniaudio's device thread (or
the null backend's mixing thread), SDL's HID, joystick and raw-input
threads, Tracy's listener with `--profile`; crashpad's handler is a
separate process.

## Building

Windows, from the repository root (needs Visual Studio 18 with MSVC 14.51):

```bat
scripts\with-msvc.cmd cmake --workflow --preset msvc-dev
```

Headless runs of a game module, with per-tick hashes, replays and the
determinism cross-check:

```bat
out\build\msvc-dev\bin\se_replay --module kernel_test --ticks 600 --fixed-frame-dt 16.6667 --hash-out hashes.txt
```

The rampage module runs the sandbox scene (assets/scenes/sandbox.scene.json
by default, or `--scene FILE`) or a legacy scenario with its recorded input,
and writes the legacy projection format; refcmp compares it with the
reference's golden (positions within 1e-3 px, rotations within 1e-3 degrees,
liveness and stats exact). From the repository root:

```bat
out\build\msvc-dev\bin\se_replay --module rampage --legacy-scenario tests/fixtures/reference/effects.scenario.json --inputs tests/fixtures/reference/effects.seinput --ticks 3600 --projection-out effects.seproj
python tools/refcmp/refcmp.py effects.seproj tests/fixtures/reference/effects.seproj.gz
```

Add `--legacy-rules v1` and use tests/fixtures/reference/v1/ to compare with
the legacy-ref-v1 rules (R1-R10). CTest runs these as `ref.sandbox.NAME` and
`ref.sandbox_v1.NAME` (label `reference`).

Benchmarks (`--n`, `--iterations`, `--warmup`, `--json FILE`; `--list`
names the suites; `--max-p95-ms` is the gate the `bench.*` tests pass):

```bat
out\build\msvc-release\bin\se_bench --suite renderlist_10k
out\build\msvc-release\bin\se_bench --suite rampage_tick_5k --json rampage_tick_5k.json
```

The `bench.*` tests run only in the msvc-release preset, against
bench/thresholds.json; run them on an otherwise idle machine.

The rampage executable runs the sandbox scene in a window, drawn by
Renderer2D with the textures of config/texture.json (the title shows the
tick and the lifecycle state). The textures load through the background
lane from the pages `asset_cook` wrote into `<build>/bin/cooked` at build
time (`--cooked-root DIR` elsewhere); in dev builds a page whose PNG
changed is re-cooked on the lane, and `--hot-reload on` (the default
except in Release) reloads a PNG changed while the game runs within about
half a second. The sim holds (no ticks) until the start state's textures
and sounds are resident; the log says when it starts and stops holding
("state 'sandbox': its 5 assets are resident after ... ms").
`--lane-step-trace FILE` (test builds) writes every lane step at shutdown. `--frames N`
quits after N presented frames; the runtime's options are listed in
engine/se_runtime/include/se_runtime/app_config.h:

```bat
out\build\msvc-dev\bin\rampage --frames 600
```

rampage reads its key bindings and sound volumes from config/config.json
and its sounds from config/sound.json (M8). The sounds load like the
textures: the start state's AssetScope requests every file of the sound
table, the background lane reads each one in 1 MB steps and decodes it with
miniaudio's decoder in steps of about 1 ms (data only; ma_engine stays on
main), and the registry hands the PCM to the audio engine when it is
resident; a sound file changed while the game runs is decoded again and new
voices play the new PCM while the playing ones finish the old (ADR-087).
`--audio null` mixes on
miniaudio's null backend and plays nothing (scripts/ci.ps1 sets
`SE_AUDIO=null` for every test), `--audio none` runs without an audio
engine, `--record FILE` writes the session's replay, which `se_replay
--module rampage --inputs FILE` checks tick by tick (`--edges-out` lists
its edges and their sources), and `--no-raw-keyboard` stamps keys from
Win32 messages instead of SDL's raw keyboard. Gamepads work through the
input sampler (1 kHz); a plugged-in pad is opened when SDL reports it.
`--record`'s replay is written through the background lane as a Durable
save at shutdown, and any Durable lane job keeps a quit pending until it is
renamed into place ("quit waits for N Durable lane job(s)", "quit ready
after X ms"; ADR-089). `tools/input_probe` shows the key stamps' resolution
with a person at the keyboard (docs/gates.md). Test builds add
`--push-key FRAME:MS:KEY[:up]` (a stamped key event) and
`--durable-save-test FRAME:KB:MS` (a Durable save on a throttled disk, then
the quit).

`--pacing paced|standard|vrr|auto` chooses the FramePacer's mode:

| Mode | When | What it does |
|---|---|---|
| Paced | auto on Windows (or `--pacing paced` anywhere) when the SDL presentation patch's frame-latency waitable exists and the vblank sampler measures the window's output and agrees with its display mode within 0.5% (ADR-055, ADR-057); never while the display sleeps | waits on the waitable, then sleeps (SDL_DelayNS) to the predicted vblank minus B(next); misses from the waitable's (D3D12) or present_wait's (Vulkan) return on the vblank sampler's grid, with present statistics counted beside them (ADR-066) |
| Standard | otherwise (no patch, no sampler, a display asleep, Linux), or `--pacing standard` | the waitable when present, then SDL_WaitForGPUSwapchain; HalfRate re-presents the last composite after a frame taken at the first vblank, so that each frame shows for two (ADR-071) |
| VRR | `--pacing vrr`, where IMMEDIATE (tearing) works, patched or not | IMMEDIATE, a self-paced 120 Hz deadline (60 Hz below a 120 Hz mode), no miss logic |

HalfRate (above 100 Hz) and frame latency 2 are chosen by the pacer from
its misses and FrameCosts (with the GPU's measured g when the patch's
timestamps exist); HalfRate stays while its own presents still miss, and a
re-entry soon after leaving doubles its next stay (ADR-071). Each mode
change is logged as `pacing: <Mode> (<reason>)`. Test builds add the pacer's mode, rate, present interval,
frame latency and missed presents to the title, and `--log-level debug`
logs its statistics once a second (waits, fence waits, misses and their
source, unresolved waits) and the renderer's (quads, batches, build and
record times, the GPU time). The pacing test hooks are
`--inject-late-frame FRAME:MS` and `--inject-device-lost FRAME` (the
device status reports removed once: DeviceLost, the device recreated, the
textures re-uploaded; ADR-068). A PresentMon capture is summarised with
`python tools\presentmon\analyze.py capture.csv` (docs/gates.md has the
capture command).

The renderer's test hooks (every build but Release) write the composite
as PNG (`--readback FRAME:FILE`, or `--readback-tick TICK:FILE` for the
first frame after a tick, since the sim holds while assets load), reload a
texture on the background lane (`--reload-texture NAME@FRAME[=FILE]`) and make one step of the frame transaction fail
(`--inject-frame-fault STEP[:EVERY[:UNTIL]]`); frames count iterates.
render_shot draws a scenario offscreen at one tick, and with the legacy
build (`legacy-msvc-release`) the two can be compared by hand; from the
repository root:

```bat
out\build\legacy-msvc-release\app\legacy_replay --scenario tests/fixtures/reference/stress.scenario.json --inputs tests/fixtures/reference/stress.seinput --image-out 600:legacy.png
out\build\msvc-dev\bin\render_shot --legacy-scenario tests/fixtures/reference/stress.scenario.json --inputs tests/fixtures/reference/stress.seinput --tick 600 --out engine.png --compare legacy.png
```

Shaders are HLSL in engine/se_render/shaders/. The DXIL headers are
compiled at build time by the Windows SDK's dxc.exe; the SPIR-V headers in
engine/se_render/shaders/generated/ are regenerated with `python
tools/shaders/build_shaders.py spirv` (glslang through wsl.exe) and
committed, and `checks.shaders.spirv_fresh` in the WSL presets fails on a
stale one. The golden images (tests/golden/render/d3d12/ and vulkan/) are
rewritten with `set SE_UPDATE_GOLDENS=1` and `ctest --test-dir
out\build\msvc-dev -R Golden` (scripts/ci.ps1 clears the variable, so the
gate always checks them); review the new PNGs before committing them. The
committed legacy image (tests/golden/render/legacy/stress_600.png, on
which `render_gpu.The sandbox golden matches the legacy image, sprite by
sprite` erases and recolours every sprite) is rewritten the same way by
`render.legacy_image.*` (ADR-069).

Every run of an engine executable writes its log through the log worker to
`<pref>/logs/<app>.log` (rotated at 5 MB, 3 kept) and to stderr, and keeps
its crash database in `<pref>/crashdb`; `<pref>` is SDL's pref path
(`%APPDATA%\simple-engine\rampage\` for rampage) or `--pref-dir DIR`.
`--profile` starts Tracy (builds with `SE_TRACY`: msvc-dev, msvc-release,
clangcl-dev), which listens on 127.0.0.1:8086 for the profiler GUI.

Crash dumps (Windows; the crashpad handler writes them into the database's
reports/ directory, out of process; nothing is uploaded) decode with:

```bat
python tools\minidump\read_dump.py out\build\msvc-dev\crash_out\abort\db\reports\<id>.dmp
python tools\minidump\read_dump.py DUMP --json
python tools\minidump\read_dump.py DUMP --records 20
```

The text output lists the streams, the threads with their names, the
exception record (code and thread), the annotations (`se.app`,
`se.dump_reason` for a requested dump, `se.terminate` after
std::terminate) and the first-party streams: the breadcrumb table (one row
per ThreadRegistry slot: state, name, OS thread id, role, phase, system,
chunk and tick), one log ring per slot (0x53450100 + slot; the shared
library ring is slot 64), each record as the log worker would print it, and
the InputFrame ring (0x53450002) with each slot's seqlock stamp (2n + 2 for
a complete frame n, odd while it was being written) and its frame.
`--records N` shows the newest N records of each ring; `--json` gives
everything for scripts (tests/crash/rampage_dump_test.py shows how). In a
test build, `rampage --crash-test after-ticks:240` and
`rampage --dump-test after-ticks:240` write the M4 exit dumps.

`scripts/ci.ps1` runs the commit gate (msvc-dev, msvc-asan, clangcl-dev,
including the `lint` and `checks` test labels; `-Presets msvc-unpatched-dev`
builds and tests without the SDL presentation patch); `scripts/ci.ps1 -Milestone`
adds Debug, Release, the WSL Linux sanitizer presets and the desktop
tests. `scripts/ci.ps1 -Desktop` runs only the desktop tests (label
`desktop`: real windows on this machine's desktop, opened without taking
focus) on msvc-dev and msvc-asan. The legacy tree
builds with the `legacy-msvc-debug` and `legacy-msvc-release` presets.

Presets set every `SE_*` option a build depends on explicitly. Deleting a
cache variable from a preset does not reset it in an existing build tree,
so a value the preset used to set stays in force there: until `base` set
`SE_BUILD_PLATFORM`, the WSL clone kept the pre-M5 `OFF` and silently
skipped the platform and runtime tests. To start a tree afresh, configure
with `cmake --preset <name> --fresh`.
