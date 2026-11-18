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
| [milestone-specs.md](milestone-specs.md) | Detailed specs for M1 to M10, M12, M14 and M17a, and their integrations |
| [rules/gameplay-rules.md](rules/gameplay-rules.md) | Declared gameplay rules R1-R13 applied to the legacy reference (milestones M1b and M3a) |
| [../tests/fixtures/reference/README.md](../tests/fixtures/reference/README.md) | The legacy reference scenarios, inputs and golden projections (legacy_replay, tests/legacy, ref.sandbox) |
| [../tests/fixtures/scenes/README.md](../tests/fixtures/scenes/README.md) | The M11 scene fixtures (prefab instances with overrides) and M3's flattened output of each |
| [../bench/thresholds.json](../bench/thresholds.json) | se_bench gates and the measurements behind them |

## Modules and tools

| Target | Directory | What it is |
|---|---|---|
| se_core | engine/se_core | Kernel: World, Schedule, component registry, hashing, detmath, jobs, time (M2); the input placement rule (edge_placement.h: PlaceEdge, the Locked and Free tick windows the stepper uses) and the input buffer window (input_buffer.h) (M8, ADR-073); the PCM type the lane and the audio engine share (pcm.h, ADR-087); the dedicated driver's channels: SpscQueue, Doorbell, FaultBox, the Snapshot slot and the SnapshotChannel, and SimOwnerScope (M17a, ADR-083, ADR-085); the threaded ForkJoinPool (jobs.h: sites kept by the pool, per-worker wake words, batch claims, ChunkOutputs, the fork fault boundary, ForkStats with join tails, the 4x-median fallback, per-efficiency-class calibration, the spin, chaos mode; the built-in PrevTransformCopy is a fork site) (M14, ADR-107 to ADR-109); the Schedule keeps each system's time inside threaded forks (`SystemStats::last_fork_ns`, from `ForkJoinPool::ThreadedJoinNs()`), and the session header's flags tell an editor Play (its start in the Play-start stream) and "no session" from a game's session (ADR-111, ADR-112); DebugDraw, the World-level sink of debug primitives, and jobs::DebugDrawSlices, its per-chunk slices merged in chunk order (debug_draw.h, ADR-113) |
| se_os | engine/se_os | The headless OS layer for threads: StartThread and RoleScope (names, roles, registry slots, QoS, CPU sets), the CPU topology and pool sizes, the unbiased clock, debugger detection, ExecutableDir; depends on se_core only (M4, ADR-029); the sim thread's PreciseTimer (precise_wait.h, M17a, ADR-084); ForkWorkers, the fork pool's worker threads (fork_workers.h: role kForkWorker, Qos::kHigh, placement by efficiency class, BatteryPowered, ChaosSeedFromEnv) and ReadSystemCpuTimes (process.h) (M14, ADR-107, ADR-108) |
| se_log | engine/se_log | The log worker: drains the per-thread log rings into the spdlog file, stderr and panel sinks; the only module that includes spdlog (M4, ADR-032) |
| se_sim | engine/se_sim | Game modules, GameStateStack, SessionLog, replays, SimDriver (M2); AudioEvents and the AudioSink contract (M8); the TickGate (M9); the dedicated sim driver, headless: SimThread (role kSim; sessions built and destroyed on it, M17a's commands, the tick as a fault boundary), PacedClock (absolute deadlines, the timer-plus-spin wait with a capped margin, phi, catch-up and dilation; ManualClock for tests) and the InputFolder (M8's half-open placement, holder counts) (M17a, ADR-083, ADR-084, ADR-086); GameStateStack::Revision, which the snapshot generation follows (ADR-086); the App keeps the inline driver until M17b; M13a: ContentSource and CachedContentSource (content_source.h), the stack's StateContentGate (a push or replace waits, with every command behind it, until its state is ready; `Waiting()`), `SimDriver::CompleteBoundary`, a SessionLog that waits behind a state at the gate, GameModule::game_start_state, and content bundles (content_bundle.h: `se-content-bundle-v1`, every text a session's states loaded, served by ContentBundleSource with the session's kAssetSwap entries selecting versions) (ADR-122, ADR-123) |
| se_audio | engine/se_audio | The audio engine on miniaudio (the only module that links or includes it): one ma_engine owned by main, Music and Effects groups with the settings file's volumes, the null-backend fallback and a no-device mode for tests; refcounted PCM (WAV and FLAC through miniaudio's decoder), 256 fixed voices with scheduled starts, per-group caps that steal the oldest, the end-of-sound SPSC ring and the per-frame sweep, scopes, reloads for new voices only; the PcmClock (M8, ADR-076) |
| se_render_core | engine/se_render_core | Headless renderer half: SpriteRenderer, Camera2D, BuildRenderList; depends on se_core only (M3); both ends of the dedicated driver's snapshots: AppendRenderView (a World into a slot's render part, on the sim thread) and SnapshotRenderSource (slots into RenderLists: the newest 3, tau and alpha, per-entity interpolation, starvation, hard cuts), sharing BuildRenderList's quad code (M17a, ADR-086) |
| se_crash | engine/se_crash | Out-of-process crash capture on crashpad: Start/CrashGuard, RegisterStream, DumpWithoutCrash, the terminate annotation; nothing uploaded; a stub on Linux and in msvc-asan (M4, ADR-033, ADR-034). The watchdog skeleton: role kWatchdog, the HangDetector (boot budget), RequestDump, the shutdown deadline (M4 integration, ADR-043). M12: hangs declared after 5 periods of 1 s without progress on the unbiased clock, sampled four times a period, unless Suspended, Modal, the heartbeat or a debugger excuses them, and never counting time the watchdog itself did not sample (the process was stopped); one out-of-process dump per episode, labelled a GPU or compositor stall when main was in an SDL_GPU or pacer wait, and the log flushed after it; the episode's end logged with its length; no judging once the shutdown deadline starts (ADR-104, ADR-105); UnregisterStream, and the stream types of the session header (0x53450005) and the Play-start bytes (0x53450006) (ADR-106) |
| se_assets | engine/se_assets | The background lane (role kBackground: SPSC request and completion rings, a binary_semaphore whose 500 ms timeout drives the hot-reload scan, Urgent and Background classes with per-step QoS, bounded steps, strands, revisions, Durable jobs, the sharing-violation retry, a fault boundary per step) and its jobs (reads, SaveRequests and flushes, prefabs and scene chunks through glaze, sounds through M8's decoder, cooked `.seatlas` atlas pages one LZ4 block per step); the AssetRegistry on main (typed stores, Handle<T>, AssetScopes, hot reload, GpuResources behind an upload port) (M9, ADR-078 to ADR-082); the Text store (M13a: RequestText, a file read in 1 MB steps as a shared immutable string, hot-reloaded and frozen with prefabs; ADR-123) |
| game_rampage | games/rampage | The legacy Sandbox's gameplay on the kernel: components, systems, scenes and prefabs (assets/), legacy scenarios, projections (M3); the scene writer's kFlatten and kPrefabs modes (instances as their prefab plus RFC 7386 overrides), prefab documents, the rebase of instances on a prefab change and the removal table beside FinishLoad (M11a, ADR-114 to ADR-116); the game's flow: the Title, Gameplay, Pause and GameOver states and FlowSystem (flow.h), the scenes' prefab dependencies (content.h) (M13b) |
| se_platform | engine/se_platform | SDL3 on the main thread: SDL's initialisation, the event filter and Inbox, WakeMain, the window and its Win32 subclass (Modal, session end, power), se::fs on SDL's filesystem API (M9: stepwise FileReader with FILE_SHARE_DELETE on Windows, FileWriter, TryReplace with POSIX semantics and sharing violations as kBusy, the fault and delay injector in test builds, whose refused renames can be limited to one file (ADR-080, ADR-130)), fault dialogs off under SE_NO_MESSAGE_BOX=1 and in every test executable (SDL's assertion dialog included: a failed SDL assertion prints and aborts, ADR-070) (M5); the input sampler (role kInputSampler: SDL_UpdateGamepads at 1000 Hz on AC and 250 Hz on battery, parked on a doorbell while Suspended or Idle, inside ADR-047's window) and the open gamepads (M8, ADR-075); the display outputs (QueryDisplayConfig's current mode of the window's monitor, ADR-055) and the vblank sampler (role kVblankSampler: WaitForVBlank on the window's DXGI output, stamps in the SDL_GetTicksNS domain, a median period; a stub elsewhere) (M6b, ADR-056) |
| se_render | engine/se_render | The SDL_GPU device (D3D12 on Windows, Vulkan on Linux) (M5); Renderer2D on main: the frame transaction (FrameRecorder, its fault boundary and the --inject-frame-fault hook), GpuResources (texture handles with a magenta fallback, the retire queue, the 8 MB upload queue, device-loss release and recreation), the sprite and blit pipelines, the composite and ViewportTargets, the texture table and PNGs through stb; HLSL shaders in shaders/, SPIR-V committed in shaders/generated/, DXIL built with the SDK's dxc (M7, ADR-059 to ADR-063); `se::present` (present.h): the SDL presentation patch's frame-latency wait, present statistics, GPU timestamps, native handles, device status and present mode, all unsupported in the unpatched build (M6a, ADR-050 to ADR-053); a frame with a SwapchainOverlay holds the textures it retires until the next Frame(), so RepresentLast never draws a released one (ADR-113) |
| se_runtime | engine/se_runtime | The App on SDL's main callbacks: boot steps, the frame, the lifecycle and two-phase quit state machines, BasicInputService, the noexcept callbacks (`SE_DEFINE_SDL_APP`) (M5); the main thread's RoleScope, crash capture and its dump streams, the log worker and the watchdog in boot order (integration.h; ADR-042); the InputService (stamped edges latched per tick by the placement rule, held state, axes, focus-loss clears with source `cleared`) and the ActionMap (the game's bindings and the settings file's key-bindings as SDL keycodes, default pad bindings, unknown names a boot error), the settings file reader (user_settings.h), `--record` session replays and `--no-raw-keyboard` (M8, ADR-073, ADR-074), replacing BasicInputService; the AudioBridge (each World's audio events started at max(now, tick anchor + D), D = B + T/2 from the plan and the pacer, late and cosmetic rules, a voice scope per World), the audio engine at boot step 10 with the sound table's sounds and `--audio device|null|none` / SE_AUDIO (M8, ADR-076, ADR-077); the FramePacer over the pure PacerPolicy (Paced, Standard, VRR, HalfRate, adaptive latency, misses from present statistics or the latency wait; `--pacing`) and the PresentPort it presents through (M6b, ADR-054, ADR-057, ADR-058); RenderPresentPort, the adapter over `se::present` and Renderer2D, misses from the frame-latency wait on the vblank grid whenever it can resolve a present, with present statistics counted beside it (and checked on their own after DWM's composition offset, ADR-072) and the source only without a waitable (ADR-066), HalfRate that backs off instead of flapping (ADR-071), the GPU timings as FrameCosts' g, and device loss (DeviceLost, recreation on main, `--inject-device-lost`) (M6/M7 integration, ADR-066 to ADR-068); the background lane and the AssetRegistry in the boot and shutdown order (asset_runtime.h: an AssetScope per GameState, the hold until it is resident through SimDriver's TickGate, GpuResources behind the registry's upload port, the lane's QuitProvider for Durable jobs; `--cooked-root`, `--hot-reload`, `--lane-step-trace`) (M9, ADR-081); an App that idles (the editor in Edit mode waits for events) is no hang to the watchdog between the iterate that entered the wait and the next callback, and an AppExtension names the viewport the `--readback` hooks read (ADR-111, ADR-112); M13a: the AssetRuntime is the stack's content gate and the states' ContentSource (texts pinned per state, dependencies from GameSetup::text_dependencies, a changed text logged as kAssetSwap for the next state that loads it); GameSetup::start_state, on_focus_lost (a GameState command in the SessionLog) and the empty stack that ends the game; the game's dumps carry the session stream; a `--record` replay is submitted when the quit becomes pending and a failed Durable save at quit is logged while the game quits; the test hooks `--push-event`, `--fs-faults` and `--expect-no-threads` (ADR-123, ADR-124) |
| se_imgui | engine/se_imgui | Dear ImGui v1.93.0-docking, the only target that compiles it: ImGui's sources, imgui_stdlib and the first-party SDL3 and SDL_GPU backends, with se_imgui/imconfig_se.h as IMGUI_USER_CONFIG (GImGui thread_local, IM_ASSERT on in every build); the editor's one context on main (`se::imgui::Context`: docking, gamepad navigation, multi-viewports opt-in, settings in memory) and HeadlessFrames for tests without a window (M10a, ADR-095); the overlay crops its display to a swapchain smaller than the window ImGui saw (FitDisplayToTarget, ADR-113) |
| se_editor | engine/se_editor | The editor shell (M10a): the panel interface M10b's panels build on (panel.h: Panel, PanelContext, EditorCommands, PlayEdits, EditorViewports), the command kinds and their undo (commands.h: set, add, remove a component, create and destroy an entity, path points, groups), Selection, TextureIds, EditorStats and LogView (ADR-095, ADR-096); the EditDocument (a kEditDocument World behind its command gate, undo, coalescing, dirty), the PlaySession (Play, Play game, Pause, Step, Stop, edits as SessionLog entries, Play.Faulted with a fault boundary for its ticks and render builds), the DialogMailbox, recovery files and autosave, the test script, and the Editor: the App's AppExtension that runs them with ImGui on the single loop (ADR-097 to ADR-099). Integration: while Play runs, the process's session header describes the Play session, its InputFrames go into the dump ring and the Play-start stream holds the session's replay file without frames (its SessionLog included), so a dump replays with `se_replay --from-dump`; EditorStats carries the pool's fork sites and each phase's parallel time; Play.Faulted keeps the selection (ADR-111, ADR-112). Review fixes: in Play game the Viewport panel reports focus and hover (EditorViewports::ReportInput) and the game gets the keyboard and gamepad while the view is focused, the mouse while it is hovered; File > Open over a dirty document waits for a Save / Don't Save / Cancel prompt; a stale autosave removes its recovery file; commands take their before value when they first apply; a failed script fails the process (ADR-113). M11b: several documents (documents.h: DocumentSet of scenes and prefab documents, ids never reused, frozen during Play; the close prompt; Save All; per-document autosave and recovery with the document's kind), the content hash beside the World hash (Stop checks both), saves with prefab references and flattened Play-start bytes, EditQueue (commands bound to their document; coalesce scopes end when their panel stops drawing, their document is closed or deactivated, and at Play), prefab_commands.h (removal with what derives, Revert to prefab, override markers, the prefab rebase as one undo step), hot reload into documents through a foreign watch tag (ADR-117 to ADR-120) |
| se_editor_panels | engine/se_editor/panels | The editor's panels on se_editor's Panel interface (M10b): Hierarchy, Inspector (a reflection drawer generated from the registry's JSON hooks, the ported Stats, EffectSource and Team drawers, the Path list and the waypoint canvas whose rules become path commands), Viewport (an editor camera into a ViewportTarget shown through TextureIds, pan and zoom, picking by the broadphase scratch, exact bounds or brute force in Play.Faulted, the move gizmo and the ported overlays), Log, Profiler (the legacy axis rule), Schedule, Stress generator, Scene settings; every edit through EditorCommands (Edit) or PlayEdits (Play); the one editor module that links game_rampage (ADR-100 to ADR-103). M11b: the Hierarchy's prefab column with its override marker, the Inspector's prefab line, override markers, Revert to prefab and removal groups, StatNameCommand (a typed stat's name reaches the World's StatNames in the drawer's command group), the Stress generator's Edit-mode press as one command group of enemy-prefab instances (ADR-119, ADR-121) |
| simple_editor | apps/editor | The editor executable (M10): se_editor on se_runtime's App with rampage's GameModule and the panels of se_editor_panels docked in it (Hierarchy left, Viewport in the centre, Log, Profiler and Schedule at the bottom, Inspector, Stress generator and Scene settings right, the shell's Play and Document panels above them), opening assets/scenes/sandbox.scene.json (`-- --scene FILE`), Standard pacing at frame latency 2, raw keyboard off; `-- --editor-script FILE` (test builds) runs editor actions at frame boundaries (se_editor/editor_script.h lists them); the runtime's test hooks apply too (`--hang-test after-ticks:N` hangs inside a tick of Play); fills EditorGame's M11 members from game_rampage's scene contract, and `--scene` may name a prefab (M11b) |
| rampage | games/rampage/app | The rampage executable: the sandbox scene at 120 Hz in a window (M5), drawn textured by Renderer2D (M7) from the cooked pages of config/texture.json loaded through the background lane, hot-reloading a changed texture (M9); since M13b a session from the Title through Gameplay, Pause and GameOver (`-- --start-state ID`, `-- --scene FILE`); crashpad_handler is copied next to it |
| asset_cook | tools/asset_cook | Cooks the textures of config/texture.json into `.seatlas` pages (a header and independent LZ4 blocks of about 256 KB of RGBA8), one per texture; CMake runs it at build time into `<build>/bin/cooked` (M9, ADR-078) |
| input_probe | tools/input_probe | A window that logs each real key event's SDL stamp (SDL's raw-input thread with `--raw-keyboard on`, the Win32 message time with `off`) and its poll delay, and reports the stamps' resolution; needs a person (docs/gates.md "Win32 key stamp resolution"); only `--help` runs unattended (`tools.input_probe.usage`) (M8) |
| make_sounds | tools/audio/make_sounds.py | Writes rampage's sounds (assets/sounds/hit.wav, death.wav) by integer synthesis; `--check` is `checks.sounds_fresh` (Python 3, stdlib only) (M8) |
| se_editor_tests | tests/editor | The editor headless (`editor.*`, label unit): the ImGui context is thread_local and draws without a window, every command kind applies, reverts and redoes on the sandbox document with the document hash restored, path points, coalescing, commands as SessionLog entries, the EditDocument, Play/Pause/Step/Stop, SessionLog edits, Play.Faulted from a tick and a render throw, a Play session replayed headless to the same hashes, the DialogMailbox from another thread (TSan in WSL), recovery files through the lane, the script parser; `editor.script.*` run simple_editor headless on tests/editor/scripts (run_editor.py; the session replay through se_replay, 'Play game', the hotkeys as ImGui key events), `editor.desktop.*` in an unfocused window (label desktop) (M10a). Integration: `editor.script.panels` and `editor.desktop.panels` (every panel draws, the Viewport's target read back, the fork sites, the Play-start stream), the Play session's dump streams replayed through se_replay's dump reader, edits included (`editor.A dump taken during Play ...`), `editor.desktop.idle_watchdog` (an idle editor is no hang), and `crash.editor_play_hang` in tests/crash (a hang in Play: the dump within 6 s, replayed to the hanging tick) (ADR-111, ADR-112). Review fixes: `editor.desktop.play_game_input`, `editor.script.open_dirty` (since M11b `editor.script.close_dirty`), `editor.script.autosave_after_save`, `editor.script.failure_exit_status`, `editor.Commands of one frame undo to their own start` (ADR-113). M11b: the DocumentSet, commands bound to their document and coalesce scopes that end with their panel (documents_test.cc), undo then redo as the identity on both hashes for every command kind including StatNameCommand, removal groups, Revert to prefab and a prefab rebase, saved and changed prefabs as one undo step per document, prefab documents, flattened Play-start bytes, recovery files with prefab references (prefab_document_test.cc, on a copy of the content under the test's output directory); `editor.script.prefabs`, `documents`, `hot_reload`, `stat_names`, `stress_prefabs` (run_editor.py's `--content-copy` runs the editor on a copy of assets/ and config/) and `editor.desktop.prefabs` (two readbacks of the Viewport differ after a prefab's sprite region change) (ADR-117 to ADR-121) |
| se_editor_panel_tests | tests/editor_panels | The panels headless (`editor_panels.*`, labels unit and editor): their pure parts (canvas rules, axis rule, picking, stress plan, letterbox, gizmo, overlays) and every panel in a headless ImGui context driven by synthetic ImGui IO events, against a test World whose gate stays closed and a recording EditorCommands or PlayEdits (M10b). M11b: the Hierarchy's prefab column and marker, the Inspector's override markers, Revert to prefab and removal on the sandbox (prefab_panels_test.cc), the drawers' new-stat command group, the stress generator's one-frame press and `editor_panels.A 1000-enemy press applies in one frame` (label slow) (ADR-119, ADR-121) |
| se_audio_tests | tests/audio | se_audio and the AudioBridge mixed offline or on the null backend (`audio.*`, label unit, TSan and ASan in WSL): 8.33 ms between two ticks' sounds by onset detection, late and cosmetic rules, voice-cap stealing, the end ring and the sweep, stop, scope release and reload while a mixer thread plays, the fallback; the real device at volume 0 (label desktop) |
| se_replay | tools/se_replay | Headless runs of a game module: per-tick hashes, replays, projections; `--edges-out` lists a replay's recorded edges with their sources (M8); `--from-dump DUMP` replays a crash or hang dump's rampage session (its session header and InputFrame ring, read by dump_session.h) to main's breadcrumb tick and checks the dump's last published hash (M12, ADR-106); a dump taken during the editor's Play replays from its Play-start stream (written to `--start-scene FILE`) with the SessionLog's edits applied (ADR-111); M13a: a replay or a game's dump whose start bytes are a content bundle runs the module's states from `--start-state` (default the bundle's) on the bundle, and `--module content_test` runs the content test module (ADR-123, ADR-124); the M11/M13 integration: `--scene FILE` names rampage's Gameplay scene for those runs, and `--module rampage --start-state ID` without `--inputs` runs the states for `--ticks N` without input on the texts of `--root` (ADR-128) |
| render_shot | tools/render_shot | A rampage scenario at one tick rendered offscreen by se_render into a PNG, and the image comparison (`--compare`, the golden tolerance: the spec's three conditions, 90% of the drawn pixels, every sprite-sized object at least a quarter within, no solid block drawn in one image only, no fallback magenta; ADR-065, ADR-069); drives the legacy image comparison (whose legacy image must equal tests/golden/render/legacy/) and the rampage fault-injection tests, `render.fault.<driver>.device_lost` included (M7, ADR-065; ADR-068) |
| build_shaders | tools/shaders/build_shaders.py | Compiles the renderer's HLSL: SPIR-V headers through glslang (committed; `--check` is the WSL freshness test), DXIL headers through dxc at build time (Python 3, stdlib only) |
| compare_images | tools/render_shot/compare_images.py | Compares two PNGs with the renderer's tolerance (the same rule as render_shot, ADR-069) and writes a diff image (Python 3, stdlib only) |
| refcmp | tools/refcmp/refcmp.py | Compares two `.seproj` projections within the M3 tolerances (Python 3, stdlib only) |
| read_dump | tools/minidump/read_dump.py | Decodes a crash dump: threads, exception, annotations and the first-party streams (breadcrumbs, log rings, InputFrame ring; M12: the session header and the Play-start bytes), and lists every record that does not fit its index (a stale or torn InputFrame slot, log sequence numbers out of order) under "problems", apart from the one a running writer was overwriting (ADR-046); the crash tests' oracle (Python 3, stdlib only) |
| presentmon | tools/presentmon/analyze.py, run.ps1 | Summarises a PresentMon CSV for one process: displayed latency p50/p99, dropped and repeated frames, present intervals, with optional limits (Python 3, stdlib only; `presentmon.analyze` tests it); run.ps1 captures rampage with PresentMon (not installed; docs/gates.md) (M6b) |
| se_pacing_tests | tests/pacing | PacerPolicy, the FramePacer on a fake clock and present port, the vblank sampler on a fake source (label unit, TSan included); desktop: the sampler on this machine's outputs, SDL's refresh against the OS's, Standard without the patch, the VRR deadline, Paced's misses over 20 s on D3D12 and on Vulkan with the source they came from, and an injected late frame draining (`pacing.*`) |
| se_crash_probe | tests/crash | Registers dump streams with known contents, then crashes or requests dumps; the `crash.*` tests (label crash) check its dumps with read_dump. `crash.rampage_*` (labels desktop and crash) check the M4 exit dumps of the rampage executable, and M12's hang dumps (`crash.rampage_hang`: within 6 s of the hang, replayed by `se_replay --from-dump`; `crash.rampage_driver_stall`: the GPU or compositor stall label and the episode's end; rampage_hang_test.py). The watchdog's unit tests run a fake unbiased clock (`crash_unit.*`); tests/platform/watchdog_manual.md is the manual sleep/resume and debugger procedure. `content_probe` (tests/crash) is the content test module as a game executable: `crash.runtime_dump_with_session_log` (labels desktop and crash) dumps it after a focus-loss pause and replays the dump's session stream to its tick (ADR-124) |
| se_runtime_content_tests | tests/platform | The App headless with the content test module (`runtime.*`, label unit): the start state created once its texts are resident, a changed text applying to the next state that loads it, focus loss as a logged state command a replay repeats, the empty stack ending the game, a failed text taking the fault path; beside them `platform.quit_durability.*` (rampage headless on a throttled or held disk) and `platform.shutdown_stress` (100 headless rampage runs, label slow) (M13a, ADR-124) |
| se_present_tests | tests/present | The present API on both GPU drivers (`present.d3d12.*`, `present.vulkan.*`, chosen with SE_GPU_DRIVER): caps, GPU timing, handles and status (label gpu), and the frame-latency waits, statistics, resize and present-mode changes (label desktop) |
| SDL patch | cmake/patches | sdl3-3.4.20-se-presentation.patch: `include/SDL3/SDL_gpu_se.h` and its D3D12 and Vulkan backends, applied with `SE_SDL_PATCH` (docs/deps.md has the rebase notes) |
| se_bench | bench/se_bench | Benchmark suites and their Release gates (`bench.*`); M14's fork overhead and join tails at W = 1/3/7 (`fork_empty_w*` back to back, `fork_parked_w*` waking parked workers, `fork_50us_w*` with 50 us chunks) |
| se_jobs_tests | tests/jobs | The threaded ForkJoinPool (`jobs.*`, label unit, TSan in WSL): exactly-once chunks, visibility, no lost wake, exceptions, stop, nested forks, per-chunk outputs, PerformanceOnly, ForkStats, the fallback window, the spin, calibration, for W = 1/3/7 and eight chaos seeds; identical per-tick hashes for W = 0/1/3/7 x 8 seeds on the kernel and rampage goldens and scenarios, pile-up scenes and the RenderList (M14, ADR-109) |
| se_wake_soak | tests/jobs | The fork pool's wake-stall soak: every wake's latency with its stamps, the machine's load per 100 ms and a canary thread of the workers' kind that only sleeps on a timer; wakes over 2 ms are classified saturated, canary-late, beside other load or unexplained and reported by load band; `jobs.wake_soak` (labels slow and soak, Release only, 10 minutes) fails on an unexplained one (a quiet window, the canary on time), `--strict` on any; `jobs.wake_soak_smoke` runs 2 s everywhere (M14, ADR-110) |
| check_no_site_enabled | tests/jobs/check_no_site_enabled.py | `checks.no_fork_site_enabled`: nothing under engine/, games/, apps/ or config/ enables a ForkJoinPool site (`enabled_sites`, `EnableSite(`, a ForkSite's `enabled = true`); `checks.planted.fork_site_enabled` proves it on tests/jobs/planted (M14 exit; M16's enabling ADRs change it) |
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
| watchdog | kWatchdog | se_crash | boot step 9 | shutdown step 17 (the last) | the boot budget, hangs (one dump per episode, M12), requested dumps, the 5 s shutdown deadline |
| bg-lane | kBackground | se_assets | boot step 9, after the watchdog | shutdown step 8 (Durable jobs finish) | the background lane: reads, cooked pages, sound decoding, saves, the hot-reload scan; calls WakeMain only inside ADR-047's window |
| input-sampler | kInputSampler | se_platform | the first iterate, after the wake scope opens (ADR-047) | before shutdown step 4 | SDL_UpdateGamepads at 1 kHz (250 Hz on battery), parked while Suspended or Idle |
| sim | kSim | se_sim | `SimThread::Start` (tests; the App wires it in M17b) | Quit, `SimThread::Stop` | the dedicated sim driver: owns its session's Worlds and runs SimDriver::RunTick on PacedClock deadlines (M17a) |
| fork-worker-<i> | kForkWorker | se_core's ForkJoinPool, started by se_os ForkWorkers | when a site of the pool is first enabled (none is in a shipped build, so none starts) | the pool's destructor (with its session) | deterministic fork-join chunks, at most W_max = ForkPoolSize (M14, ADR-107) |

The editor (M10) adds no thread of its own: SDL's file-dialog thread
runs the DialogMailbox's callback (copy the paths, WakeMain, return), and
test builds play that thread with a named kLibrary thread
(`dialog-callback ... from-thread`).

Library threads (role kLibrary, no slot): miniaudio's device thread (or
the null backend's mixing thread), SDL's HID, joystick and raw-input
threads, Tracy's listener with `--profile`; crashpad's handler is a
separate process.

## The editor

`simple_editor` (M10) edits rampage's scenes: the sandbox opens in Edit
mode, with the panels docked around the Viewport: Hierarchy (left),
Inspector, Stress generator and Scene settings (right, under the shell's
Play and Document panels), Log, Profiler and Schedule (bottom). The layout
is seeded once and then kept in <pref>editor-layout.ini. The Viewport panel
shows the World through an editor camera in an offscreen target (pan with
the right or middle button, zoom with the wheel, click to pick, drag the
gizmo to move); every edit a panel makes is an undoable command in Edit
mode and a SessionLog entry in Play. The Profiler and Schedule panels show
the pacer, each system's timing ring, each phase's serial and parallel
time and the fork-join pool's sites (all disabled: no worker runs); the
Profiler's "Dump flight recorder" button writes a dump of the process as
it is.

Play serializes the document into a play World (every Play is a serializer
round trip), Play game runs rampage's own session on it: it starts at the
Title, Enter plays the document (the Gameplay state's scene is the
Play-start bytes), and its states load their scenes through the background
lane and the content gate as the runtime's do (no tick before a state's
texts and assets are resident; a text that fails faults the session).
Pause, Step and
Stop take effect between frames and are logged with their tick, and Stop
leaves the document's hash unchanged. Edits during Play go to the play
session's SessionLog, never to the document. A throw in a play tick or in
the play World's render build enters Play.Faulted: the World stays
inspectable, its replay, SessionLog and breadcrumbs go to
<pref>faults/. Dirty documents autosave to <pref>recovery/ after 2 s
without input and are offered back at the next start; the quit prompts
about unsaved changes and saves through the background lane. Hotkeys:
Ctrl+S, Ctrl+Z, Ctrl+Y, F5 (Play or Resume), Shift+F5 (Stop), F10 (Step).

Documents and prefabs (M11b): File > Open adds a document (a scene, or a
prefab such as assets/prefabs/enemy.prefab.json, which opens as a World
with its one root entity), the Document panel's tabs switch between them,
File > Close asks about unsaved changes, Save All saves every dirty one and
the quit prompt lists them all. Each document has its own undo stack.
Saves keep the prefab references (an instance is written as its prefab
plus overrides); the Play-start bytes stay flattened. The Hierarchy shows
each entity's prefab (with "*" for an instance that has overrides), the
Inspector marks the components that differ from the prefab and reverts
them ("Revert to prefab"); removing a component removes what derives from
it. A prefab saved in the editor, or changed on disk, updates the open
scenes' instances in one undo step each ("Prefab changed: <ref>"); a
document whose own file changes on disk reloads when it is clean and asks
(Reload, Keep mine) when it is not; during Play these wait for Stop.
`simple_editor --audio null -- --scene assets/prefabs/enemy.prefab.json`
opens a prefab directly. The quit's Save All saves every dirty document as
a Durable lane job and waits for all of them; one that fails stays dirty,
shows its error and cancels the quit while the others are saved
(`editor.script.quit_durability`).

The editor and rampage load a scene into the same World:
`editor.The editor and the runtime load identical Worlds` compares the
editor's document with the Gameplay World of a headless rampage on the
same file, for every scene fixture (tests/fixtures/scenes) and the
sandbox, and `rampage.plays_an_editor_saved_scene` plays a scene the
editor saved with prefab references in rampage (`-- --start-state gameplay
--scene FILE`) and compares its first 600 tick hashes with se_replay's on
the editor's Play-start bytes (`se_replay --module rampage --start-state
gameplay --scene FILE --root DIR --seed 1 --ticks 600`).

<pref>sessions/last-play.sereplay replays headless:

```bat
out\build\msvc-dev\bin\simple_editor --audio null
out\build\msvc-dev\bin\se_replay --module rampage --inputs %APPDATA%\simple-engine\simple_editor\sessions\last-play.sereplay --start-scene start.scene.json
```

In Edit mode the editor idles after 1 s without input (it waits for
events; the watchdog does not take that for a hang). While Play runs, a
dump of the process (a hang, a crash, Play.Faulted, the flight-recorder
button) carries the Play session: the session header, the InputFrames and
the Play-start stream with the document as Play serialized it and the
edits made since. `se_replay --from-dump DUMP` replays it to the tick main
was in (ADR-111).

Test builds run a script of editor actions at frame boundaries, in a
window or headless; se_editor/editor_script.h lists the actions and
tests/editor/scripts has the ones the tests use (run_editor.py runs them:
`editor.script.*` headless, `editor.desktop.*` in a window that never takes
the focus). A script never uses real input:

```bat
out\build\msvc-dev\bin\simple_editor --audio null --no-activate -- --editor-script tests/editor/scripts/panels.editor
out\build\msvc-dev\bin\simple_editor --audio null --headless --fixed-frame-dt 8.3333 -- --editor-script tests/editor/scripts/play_session.editor
```

## The fork-join pool

`jobs::ForkJoinPool` (se_core, M14) runs a system's `ParallelFor` chunks on
the caller and on up to W_max fork workers (role kForkWorker, started by
se_os when a site is first enabled). Every fork site ships disabled, so a
shipped build starts no worker and every fork runs inline;
`checks.no_fork_site_enabled` keeps it so until M16 enables sites one by
one. The results never depend on W: `jobs.*` compares per-tick hashes for
W = 0, 1, 3 and 7.

- Chaos mode permutes each fork's claim order from a seed and injects
  yields, short delays and late wakes (ADR-109): `set SE_CHAOS_SEED=0x51C3`
  (decimal or 0x hex) turns it on for an App and for the tests' pools; the
  tests run eight seeds. Results never depend on it.
- `se_replay --module rampage --jobs 3 ...` runs a session on three workers
  with every site enabled; the hashes equal the inline run's.
- `ctest --test-dir out/build/msvc-dev -R "^jobs[.]"` runs the pool's
  properties and the determinism matrix (TSan in the WSL preset);
  `out\build\msvc-release\bin\se_wake_soak.exe --strict` is the 10-minute
  wake soak for an idle machine (docs/gates.md).
- In the editor, the Profiler and Schedule panels list the pool's sites
  (state, forks, inline forks, join p99.9, late and missed wakes,
  fallbacks) and each phase's parallel time, which is 0 while the sites
  are disabled.

## The game flow headless

`rampage.flow.*` (tests/rampage/flow_test.cc) drive the module's stack one
InputFrame at a time (tests/rampage/support/flow_session.h, FlowHarness,
with a CachedContentSource over the repository). The golden
`determinism_rampage_flow` replays tests/fixtures/flow/flow.seinput, a
session through every state on tests/fixtures/flow/doomed.scene.json,
under 30, 60, 144 and 160 Hz clocks; the input file is the output of
`rampage_flow_input_gen OUT_FILE` (tests/rampage/flow_input_gen.cc, through
StampedInput), which `rampage.flow_input_fixture` checks. Through the
executable (tests/rampage/flow_app_test.py, keys pushed with `--push-key`):
`rampage.headless_flow` records a session through every state and
se_replay reproduces its hashes from the replay's content bundle;
`rampage.desktop.flow` reads the window back in each state (an unfocused
window); `rampage.frames_300_title` is `rampage.frames_300` from the Title.

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

The rampage executable runs the game in a window, drawn by Renderer2D with
the textures of config/texture.json (the window title shows the top state's
id, the tick and the lifecycle state). A session starts at the Title (the
legacy MainMenu's wallpaper; no text until M19): Enter plays the Gameplay
scene (assets/scenes/sandbox.scene.json, or `-- --scene FILE`), Escape
opens the Pause (the wallpaper as a dark veil over the paused game) and
Escape there resumes, Space (Jump) in the Pause returns to the Title; when
the player dies the GameOver (the wallpaper in red) follows, where Enter
retries and Escape returns to the Title; Escape at the Title ends the game.
Losing the window's focus while playing opens the Pause. The rules are R14
(docs/rules/gameplay-rules.md); the gamepad's start, back and south buttons
are Enter, Escape and Jump. `-- --start-state sandbox` runs the one-World
sandbox state instead (the editor's Play, se_replay and the goldens use it;
`-- --start-state gameplay` skips the Title). Each state's scene loads as
a text through the background lane with the prefabs it names before the
state is created (M13a). rampage keeps no game save. The textures load
through the background lane from the pages `asset_cook` wrote into `<build>/bin/cooked` at build
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

Hangs (M12): the watchdog declares one when main's iterate counter has not
moved for 5 periods of 1 s (the unbiased clock, so system sleep does not
count) and nothing excuses it (Suspended, Modal, known long work, a
debugger). The log shows `hang N: main made no progress for X s at tick T`
(or `hang N: GPU or compositor stall, ...` when main was inside an SDL_GPU
or pacer wait), the dump carries the same text as `se.dump_reason`, and
`hang N ended: main made progress again after X s` follows when main runs
again. A dump also carries the session header (0x53450005: module, scene,
seed, tick rate, build, start tick, the last tick's hash), so the session
replays headless from it:

```bat
out\build\msvc-dev\bin\se_replay --from-dump DUMP --hash-out hashes.txt
```

It reaches main's breadcrumb tick and checks the dump's hash. In a test
build `rampage --audio null --hang-test after-ticks:240` loops forever in a
system at tick 240 (`:for-ms:MS` for a while, `:in-driver` inside a driver
wait); tests/platform/watchdog_manual.md has the manual sleep and debugger
checks. The same hook hangs the editor inside a tick of Play
(`simple_editor --audio null --hang-test after-ticks:240 -- --editor-script
tests/editor/scripts/play_hang.editor`, the test `crash.editor_play_hang`):
the dump then carries the Play-start stream (0x53450006), and `se_replay
--from-dump DUMP --start-scene start.scene.json` starts from it. A dump
of 'Play game' (`play_game_hang.editor`, `crash.editor_play_game_hang`)
carries the session's content bundle instead, and `se_replay --from-dump
DUMP` runs the game's states from it (the Gameplay scene under
`play-game.scene.json`). An App that waits for events (the editor idle in
Edit mode) is not a hang; a dump taken then says in its session header that
no session runs. A state whose scene is read but does not load (not JSON, an
unknown component, a bad prefab) is not created: the game takes the fault
path with the file's path, line and column.

A game's states build their Worlds from texts (scenes, prefabs) the
background lane reads (M13a): a state named in a push or replace is
created only once its texts (`StateAssets::texts` and what
`GameSetup::text_dependencies` names for them) and its assets are
resident, and no tick runs before. A text changed on disk never changes a
running World; the next state that loads it reads the new version, and the
SessionLog records the swap. When the states loaded any text besides the
scene file, a `--record` replay's start bytes, and the session stream a
game's dump carries (0x53450006), are a content bundle
(`se-content-bundle-v1`: every version of every text the session read);
`se_replay --inputs FILE` and `se_replay --from-dump DUMP` then run the
module's states from it (`--start-state ID`, default the bundle's start
state). `rampage --record` submits its replay as soon as the quit is
pending, so the quit waits for the write; a write that fails is logged and
the game quits anyway (the editor cancels its quit instead).

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
