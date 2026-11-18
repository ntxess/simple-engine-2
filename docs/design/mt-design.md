# Multithreaded engine design (recommended design)

Generated from the 2026-11-08 design workflow (final editor output after the red team). This is the specification the redesign implements. Published page: https://claude.ai/artifact/4NwGj9Td5wNJbT7B4nNEE7 (private). Identifiers are written as in the design; code uses Google C++ style (see docs/style.md).

## Headline

Recommended: one owner thread (the SDL main thread) with deterministic fork-join workers, a background I/O lane, and small single-purpose helpers at the edges (log worker, watchdog, input and vblank samplers). Do not split physics and rendering into separate threads. At the stated targets this beats the split, it absorbed a 39-finding red team without changing the ranking, and it keeps a contained route to a dedicated simulation thread if measurements ever call for one.

## Recommendation

Adopt the single-thread-control family as the engine's threading model. The panel chose it 3 of 3, and the red team's 39 findings were absorbed without changing the ranking. Fold in these required changes.

Loop and catch-up
1. Budget-sized cap.
   - k_rt = ceil(T/dt).
   - k_hi = floor((T - R_hi - g_hi - m)/S_hi), using p99.5 costs over 4 s.
   - k_95 is the same formula with p95 costs over 2 s.
   - m = max(1.5 ms, timer p99 + 0.5 ms).
   - Normal: k_max = clamp(max(k_rt, k_hi), 1, min(8, k_rt + 1)).
   - Overloaded: clamp(k_95, 1, k_rt) in game builds, clamp(k_95, 0, k_rt) in editor Play.
   - Debt not repayable within 0.25 s becomes counted, displayed dilation.
   - Enter Overloaded after 2 consecutive missed vblanks with debt, or k_95 < k_rt for 1 s.
2. Locked cadence when the display period is within 0.5% of an integer ratio of dt (60, 59.94, 120 Hz, and VRR mode): exactly k ticks per elapsed vblank, rendered at alpha = 1. Free cadence with interpolation elsewhere. Plan the next frame's ticks before the late-latch sleep.
3. Spike sources off the tick: decoding, parsing, saves and flushes go to the lane; mass spawns are spread across ticks; amortised systems stagger work by Guid hash.

Presentation
4. A small patch on the vendored SDL3, offered upstream. It adds:
   - a DXGI frame-latency waitable with SetMaximumFrameLatency(1 or 2);
   - present statistics;
   - GPU timestamp queries;
   - native handles for Tracy;
   - a device-removed query;
   - VK_KHR_present_id/present_wait.
   Unpatched builds fall back to Standard pacing with GPU time unknown.
5. Pacing modes.
   - Paced is the Windows runtime default on fixed-refresh displays: wait on the waitable, then sleep to the window output's predicted vblank minus B(steps). The vblank sampler uses IDXGIOutput::WaitForVBlank, and misses come from present statistics.
   - VRR mode is a user setting.
   - Standard is used for the editor and Linux.
   - HalfRate is a state with hysteresis.
   - Frame latency adapts between 1 and 2; the editor defaults to 2.
6. Detect the Windows modal loop with a window subclass. While Modal: no waits, no B adaptation, no Overloaded entry.

Lifecycle and durability
7. Two-phase quit.
   - QUIT sets quitPending.
   - The prompt and Durable saves run over normal iterates; only then return SDL_APP_SUCCESS.
   - The 5 s deadline starts after the drain.
   - On WM_QUERYENDSESSION or TERMINATING, write recovery files synchronously.
8. All four SDL callbacks are noexcept with catch-alls. *appstate is set only after a complete init, SDL_AppQuit accepts NULL, and audio falls back to a null device.
9. A NULL swapchain texture is a skipped present, never a state change. Suspended is entered only on MINIMIZED, HIDDEN or OCCLUDED (plus the Wayland suspended state) and iterates at 10 Hz. DeviceLost is entered only on a device-removed status.
10. Frame recording is a transaction: everything that can throw runs before any acquire, and a guard submits, never cancels after acquire, and owns the fence.
11. Editor protection: a recovery file on every Play, idle autosave, release-build hang dumps, and faulted Play Worlds frozen read-only with brute-force picking.

Resource lifetime
12. Every SDL_ReleaseGPU* goes through a retire queue that runs after submit. ImTextureIDs are resolved from handles every frame and never stored. Resource-releasing editor commands apply at the next frame start.
13. Uploads get their own command buffer, submitted every iterate; a texture is resident when its fence signals.
14. Voices hold PCM by reference count. Scope release stops its voices first. Sounds start at tick anchor + D; late gameplay sounds play instead of being dropped.

Threads at the edges
15. An SDL event filter diverts off-main immediate events into the inbox. The inbox is drained by swap at iterate start, never calling SDL under its lock. Engine threads push only wakeMain() user events.
16. File-dialog callbacks only fill a refcounted mailbox and wake main. Quit waits while a dialog is open.
17. One background lane:
   - Urgent and Background classes;
   - strands by destination path, unique temp names, revisions;
   - Durable jobs;
   - steps of about 2 ms that check stop_token.
   Main talks to the lane and the log worker only through SPSC rings and takes any other shared lock with try_lock.
18. Gamepads sampled at 1 kHz (250 Hz on battery). Input edges are placed on ticks by timestamp (raw keyboard in runtime builds), with a 6-12 tick buffer window.

Determinism and diagnostics
19. GameState changes and every play-World mutation the Schedule did not produce apply at tick boundaries and are recorded in a SessionLog. Gameplay assets freeze during Play. Runtime Guids derive from (parent Guid, tick, spawn index).
20. First-party deterministic sin/cos/atan2 in simulation code, and _set_FMA3_enable(0) at startup.
21. Out-of-process crash and hang dumps (Crashpad, Windows and Linux).
   - Registered streams: breadcrumbs {role, phase, system, tick, chunk:16}, log rings, the InputFrame ring, the SessionLog and a pacing flight recorder.
   - Hang = 5 consecutive 1 s periods without progress on an unbiased clock, armed after the first iterate.
   - Never DebugBreak.
   - The sim tick goes on every log line and Tracy zone.
22. A Dev build preset (optimized, asserts on, _ITERATOR_DEBUG_LEVEL=0 everywhere) is the editor default. Budgets are checked in Dev and Release.

Fork-join and contracts
23. Ship W = 0. Enable a site only after all of these:
   - se_bench records a miss;
   - TSan chaos (with pile-up scenes) is green;
   - overhead is at most 10%;
   - a 10-minute soak on the weakest machine shows join p99.9 and max no worse than inline.
   Further rules:
   - Inline fallback after a slow join.
   - Pool = physical cores - 2 (- 1 on homogeneous 8+ core CPUs), with E-cores for claim-based sites.
   - At least 4 chunks of at least 10 us per participant.
   - Per-chunk outputs and sinks; declared read/write sets.
   - A load-claim-wait worker loop.
24. CI-checked seams:
   - World or ParallelView for systems;
   - a RenderSource for rendering;
   - InputFrames by value;
   - banned includes;
   - owner-thread asserts;
   - thread_local GImGui;
   - ENTT_USE_ATOMIC with pre-created storages.
25. Re-measure on the real stack before freezing thresholds:
   - the 7700X;
   - 4-core and 2P+8E laptops on battery;
   - two monitors;
   - a VRR panel;
   - Mesa/Wayland and NVIDIA/X11.
   Measure latency with PresentMon displayed time plus a photodiode spot check.

Keep the dedicated-threads design as the documented escalation; build it only if a flip condition is measured. Never build the task graph at this scale.

## Answer

What the design is. The SDL main thread owns everything that changes: the window, input, every World, the editor and ImGui, the GPU and audio control. Each frame it:
1. takes the pumped input and applies queued commands;
2. runs the fixed 120 Hz ticks that are due: exactly 2 per frame at 60 Hz and 1 at 120 Hz (Locked cadence, shown at alpha = 1), or 5 per 6 frames at 144 Hz (Free cadence, interpolated). State changes and editor edits land on tick boundaries;
3. builds a render list from the read-only World;
4. records and submits the frame in one exception-safe transaction, then retires released GPU objects;
5. waits for the swapchain's frame-latency signal, then until just before the next vblank of its own monitor, sized for the ticks it has already planned.

The other threads never touch world state:
- Fork-join workers split heavy loops (broadphase, narrowphase, sorts, culling) with results identical for any worker count. A loop is enabled only after a benchmark shows a miss and a 10-minute soak shows its tail is no worse than inline.
- One background lane does I/O and decoding in steps of about 2 ms, urgent jobs first. Saves the user asked for cannot be cancelled.
- A log worker drains per-thread log rings. A watchdog asks an out-of-process crash handler for a dump if main hangs.
- An input sampler polls gamepads at 1 kHz. On Windows a vblank sampler timestamps the vblanks of the window's monitor.
- Library threads: miniaudio, Tracy, and SDL's raw-input, joystick and file-dialog threads.

Your five goals:
- CPU utilization: partly met. Data-parallel work can use up to 8 cores on the 7700X, plus E-cores on hybrid laptops, but serial physics and serial rendering never overlap.
- Frame latency: not met, by construction. Physics runs before the frame that shows it, so a 25 ms tick costs one missed vblank. Mitigations: a budget-sized cap fixes the measured 30 Hz lock-in, Locked cadence removes the drift frame that locked a fixed late latch, spike sources leave the tick, and a frame-latency waitable stops the swapchain from silently adding a refresh.
- Independent rates: partly met. There are exactly 120 ticks per simulated second at any display rate, with identical results from 30 to 160 Hz. In wall-clock time the ticks bunch at frame start, and at 59.94 Hz the simulation runs 0.1% slow (counted).
- Scalability: partly met. Parallel phases spread across cores, but serial rules stay on main.
- Isolation: partly met. Responsibilities are separated by contract and by the link graph. One thread means a hang or a breakpoint stops both halves; durable saves, recovery files and out-of-process hang dumps cover that.

Run-time cost. These numbers are from a synthetic rig on your 7700X. At the targets the loop used 5.9-23% of one core, with no world locks and no snapshots. At the shipped default of zero workers, only log records cross threads during a tick. A fork costs 5-25 us at p50 (measured), so a loop is forked only when each participant gets at least four chunks of 10 us or more.

Compared with the single-threaded plan: this is that plan, hardened. The red team changed the edges, not the core. It added:
- a two-phase quit with durable saves;
- a GPU retire queue with per-frame texture resolution;
- an exception-safe frame transaction;
- tick-boundary state changes with a session log;
- out-of-process crash capture;
- a small patch to the vendored SDL for presentation signals (waitable, present statistics, GPU timestamps);
- a per-monitor vblank sampler, a VRR mode, a gamepad sampler and a Dev build preset.

Compared with a physics/render split, it reaches the screen 4-6 ms sooner at p50 in the rig (to a simulated vblank: 24.0 vs 29.6 ms at 60 Hz, 10.7 vs 14.9 ms at 144 Hz) and has far fewer parts. The split won only beyond your targets or under spikes, and even then world motion froze in both designs. Most presentation findings would hit the split too.

Two findings weigh against the single owner specifically:
- Debug editor builds run EnTT-heavy code 63-111x slower, so a Dev preset is the default for editor work.
- Wayland FIFO stalls freeze main.
Both feed the documented flip conditions. If the real engine on a weak machine lands in the losing zone, swap the tick driver for the dedicated-threads design; systems and rendering do not change.

## Multithreading Verdict

No. A physics thread beside a render thread is the model behind the five goals, and it does not beat one paced owner thread for these targets on the 7700X. Its end-state price is not worth paying.

The best multithreaded design here is one owner plus helpers:
- deterministic fork-join inside heavy loops;
- a background I/O lane;
- small single-purpose threads at the edges (log worker, watchdog, input and vblank samplers);
- library threads and an out-of-process crash handler.
Many cores work on data-parallel loops; one owner holds shared state.

Evidence (rig measurements unless noted):
- At the targets the loop used 5.9-23% of one core.
- With no spikes there were 0 hitches at 59.9 Hz up to N=50k, and at 144 Hz up to N=10k. The red team measured 0 hitches at 144 Hz with one tick per frame at N=35k and 40k.
- It presented input 4-6 ms sooner at p50 than two threads paced the same way: 24.0 vs 29.6 ms, 10.7 vs 14.9 ms, and 11.9 vs 17.6 ms late-latched (to a simulated vblank).
- Two threads won only beyond the targets or under stress:
  - With 25 ms spikes: 0 vs 2 hitches per run, and world motion froze in both designs.
  - At 144 Hz with N=50k: 0 vs 17 hitches. Capping the single loop at one tick per frame still gave 17 and 5 hitches (red team). Only forks or fewer entities fix that load, which is 10x the collider target.
- The 30 Hz lock-in at N=50k was the catch-up policy: a 3-step cap gave 2 hitches against 115.
- A fixed late latch on a 59.94 Hz display at N=30k locked the same way: 969 hitches in 40 s (red team). One more tick of headroom gave 0. Locked cadence and planning before the sleep remove that drift frame.

Per goal:
- CPU utilization pays only as fork-join: 3.0-3.6x on the tick, with identical results. A second owner thread would idle about 96% of the time.
- Keeping rendering unblocked by physics is not worth a second thread at the targets. The second thread holds cadence through spikes, but the world still freezes, and it costs latency every frame.
- Independent rates are delivered in simulated time.
- Scalability is delivered for parallel phases.
- Isolation is delivered by contract; execution isolation is the gap, covered by durable saves, recovery files and hang dumps.

When this flips, the answer is dedicated-threads, never the task graph at this scale. It flips if any of these is measured on the real stack:
1. The post-fork serial tick S on the weakest supported machine falls in (T - R - m)/k < S <= dt - 0.5 ms. Computed with m = 1.5 ms, that is:
   - about 4.4-7.8 ms for a 144 Hz game (R = 1 ms);
   - 2.4-7.8 ms for a 144 Hz editor (R = 3 ms);
   - 7.1-7.8 ms at 60 Hz (k = 2).
   For the editor, use Dev-preset numbers: in the red team's Debug build the rig tick was 3.0x slower and EnTT views 63-69x slower.
2. A mode needs sim cadence that survives window-thread blocking: rollback or lockstep netcode, audio-locked timing, or Linux/Wayland FIFO stalls seen on target hardware.
3. The editor must stay responsive through hung or long Play ticks, and the Dev preset, the editor UI floor, durable saves and hang dumps prove insufficient.
4. 240 Hz or higher displays with ticks over about 1.5 ms become a target.

## Overview

The design in one paragraph. One owner thread with helpers. The SDL main thread owns everything mutable:
- the window, its subclass procedure, the event pump and input;
- every World, and with it every EnTT structural change;
- the editor and ImGui;
- every SDL_GPU command buffer, the swapchain, the retire queue and the upload queue;
- the asset tables, the voice table and audio control.

Each SDL_AppIterate:
1. Drains the inbox and file-dialog mailboxes. SDL has already pumped events into SDL_AppEvent.
2. Reads fences and present statistics, makes finished uploads resident, drains lane completions, and applies frame-start commands (Play/Stop, resource-releasing edits).
3. Runs 0 to k_max fixed 120 Hz ticks under the budget cap; GameState changes and SessionLog entries apply at tick boundaries.
4. Builds a RenderList from a read-only RenderSource (today the const World), at alpha = 1 in Locked cadence or acc/dt in Free cadence.
5. Runs the editor UI.
6. Records and submits the upload and frame command buffers in one exception-safe transaction, then runs the retire queue.
7. Plans the next frame's ticks and waits: first the frame-latency waitable, then the paced sleep. SDL pumps input after the wait.

Parallelism comes in these forms; none shares world state:
- Fork-join workers in named fork sites, one per physical core (E-cores for claim-based sites). W = 0 ships at the targets.
- One background lane with Urgent and Background classes, in steps of about 2 ms.
- A log worker, a watchdog, an input sampler (gamepads at 1 kHz) and, in Windows Paced mode, a vblank sampler.
- Library threads and the out-of-process Crashpad handler.

There is no world lock, no snapshot ring and no cross-thread lifecycle for world state, so every SDL3, SDL_GPU, ImGui and EnTT thread rule holds by construction. Each remaining cross-thread edge has one named mechanism:
- the event filter and inbox;
- dialog mailboxes;
- SPSC rings for the lane, logs and audio;
- the retire queue for GPU objects;
- reference counts for PCM.

Why not a physics/render split. Under SDL_GPU no design gets a real render thread: swapchain acquire, the presenting command buffer and the ImGui frame are pinned to the window thread (dep/imgui-1.92.5/backends/imgui_impl_sdl3.cpp:970-1014; dep/imgui-1.92.5/backends/imgui_impl_sdlgpu3.cpp:400-404, 697-734). The real alternative is a simulation thread with snapshots. Measured, it buys cadence through spikes and beyond-target loads. It costs 4-6 ms of input latency every frame, plus a snapshot schema, a Play-mode editor mirror and more lifecycle. At the targets, main's critical path is about 2.2 ms of a 16.7 ms frame.

What it keeps open. These seams make a later simulation thread a driver change:
- ticks run only through SimDriver::tickInline;
- systems take World or ParallelView;
- the renderer reads a RenderSource;
- input arrives as InputFrames by value, and every other play-World input arrives through the SessionLog.

Differences from the panel's proposal:
- Stepping: the budget cap, the Overloaded state, Locked cadence and next-frame planning.
- Events: an event filter and a mandatory inbox.
- Durability: a two-phase quit with Durable saves, recovery files, idle autosave and out-of-process hang dumps.
- Fork-join: W = 0 by default; fork sites gated by se_bench, ForkStats, a tail soak and TSan chaos; per-worker wake-up.
- Lane: one thread with job classes and strands.
- Rendering and GPU: thread_local GImGui, per-frame texture resolution, a frame transaction and a retire queue.
- Inspection and diagnostics: faulted-session inspection, breadcrumbs, log rings and a flight recorder.
- EnTT: ENTT_USE_ATOMIC with pre-created storages.
- Presentation: a small SDL patch (waitable, present statistics, timestamps), a per-monitor vblank sampler, VRR mode, adaptive frame latency and HalfRate hysteresis.
- Input and audio: timestamp input placement and a gamepad sampler; tick-anchored audio with refcounted PCM.
- Seams and builds: the RenderSource seam, a SessionLog, a Dev build preset and deterministic trig.

## Threads

### main (SDL main thread)

**Name:** main (SDL main thread)

**Count:** 1

**Owns:** SDL video, the window, its Win32 queue and its subclass procedure (Modal, session-end and power messages), the event pump, the inbox and file-dialog mailboxes. InputService. The GameStateStack and every World, and therefore every EnTT structural change. The Schedule, FixedStepper and SessionLog. The ImGui context and the whole editor. Renderer2D: the SDL_GPU device, the frame and upload command buffers, swapchain acquire and present, the retire queue and the texture resolver. AssetRegistry tables and the upload queue. Every ma_engine and ma_sound call and the voice table. The replay recorder and flight recorder. Pacing and overload accounting. The caller role in every fork.

**Runs At:** One SDL_AppIterate per presented frame, at 60-160 Hz. Locked cadence at 60, 59.94 and 120 Hz and in VRR mode: exactly 2 ticks per vblank at 60 Hz and 1 at 120 Hz, at alpha = 1. Free cadence elsewhere: 5 ticks per 6 frames at 144 Hz, 8 per 11 at 165 Hz. HalfRate presents every second vblank above 100 Hz. The idle editor iterates only on events and wakes; Suspended runs at 10 Hz. Busy time at the targets was 5.9-23% of one core in the rig (measured, no GPU or ImGui) and about 2.2 ms per 60 Hz game frame in the timeline (partly estimated).

**Wakes On:** The pacer's wait at the end of each iterate:
- Paced: the frame-latency waitable, then SDL_DelayNS to the predicted vblank of the window's output minus B(next steps).
- VRR: the waitable, then an absolute 120 Hz deadline.
- Standard: the waitable (frame latency 1 or 2), then SDL_WaitForGPUSwapchain.
SDL then pumps events and calls the next iterate. Idle editor: SDL_HINT_MAIN_CALLBACK_RATE=waitevent, woken by input or wakeMain(). Suspended: rate 10. Windows drag: SDL's WM_TIMER drives iterates and the Modal flag removes the waits.

**Never Touches:** No data is off limits; its rules are about waiting and locking. It never spins. It never blocks on the lane, the log worker, the watchdog, the samplers, miniaudio or a dialog thread. It waits on workers only inside a fork join. It takes shared locks only with try_lock. It never calls SDL while holding the inbox lock. It never calls SDL_RunOnMainThread, and it never pushes immediate-dispatch SDL events.

**Why:** SDL3, SDL_GPU and imgui_impl_sdl3 already pin windows, events, mouse state, the swapchain and the ImGui frame to this thread (dep/imgui-1.92.5/backends/imgui_impl_sdl3.cpp:970-1014; dep/imgui-1.92.5/backends/imgui_impl_sdlgpu3.cpp:400-404, 697-734). Giving it every World too removes world locks, snapshots and cross-thread lifecycles, the bug class behind today's races (core/Engine.cpp:181-183 against :204-208).

### fork-join workers

**Name:** fork-join workers

**Count:** Pool size W_max = physical cores - 2, capped at 15. Homogeneous CPUs with 8 or more cores reserve only 1. That gives 7 on the 7700X, 2 on a 4-core laptop, 8 on 2P+8E and 12 on 6P+8E. PerformanceOnly sites (barrier-heavy, such as Box2D) use the P-cores on main's last-level cache minus 1: 7, 2, 1 and 5. SMT siblings are excluded. The shipped default at the targets is W = 0: no threads exist until a site is enabled, and W stays 0 below 3 physical cores.

**Owns:** Nothing persistent. During a fork it owns its claimed chunks, their per-chunk outputs (growable from per-chunk arenas, concatenated by main in chunk order), its per-chunk DebugDraw slice, and its 64-byte stats and breadcrumb slots.

**Runs At:** Only inside enabled fork sites: at most 8 forks per tick and 4 per frame, with at least 4 chunks of 10 us or more per participant. Parked otherwise.

**Wakes On:** Its own padded wake word. Main does fetch_add(release) and notify_one for the participants - 1 workers a fork needs. The worker loop is: load the wake word, try to claim, then wait(old), so no bump is lost. After a join it spins at most 20 us when another fork is predicted (never on battery), then parks.

**Never Touches:** Any SDL API or ImGui (GImGui is thread_local). The EnTT structural API: create, destroy, emplace, remove, patch, a non-const view of a missing storage, groups, ctx emplace. The dispatcher, World-level sinks, the AssetRegistry, miniaudio and logging configuration. Any component outside the site's declared read and write sets, or outside its chunk's write range (ParallelView asserts both in debug builds).

**Why:** Broadphase, narrowphase, sorts, culling and vertex generation are data-parallel. Fork-join puts idle cores on them without a second owner or timeline. It measured 3.0-3.6x on the tick with bit-identical results. Claimed chunks tolerate cores of unequal speed, so E-cores can help claim-based sites.

### background lane

**Name:** background lane

**Count:** 1 on every machine

**Owns:** File I/O and decoding: LZ4 atlas pages, non-streamed PCM, and glaze parsing of scene chunks, prefabs and settings. Editor atlas packing and import-time PNG decoding. Save, settings, recovery and autosave writes (unique temp file, revision check, rename). Screenshots and thumbnails. The 500 ms hot-reload scan. Replay, SessionLog and flight-recorder flushes. It owns each buffer until it pushes the result into the completion ring.

**Runs At:** On demand, in steps of about 2 ms or less (estimated, checked in se_bench), with a stop_token check between steps. Urgent jobs (loads someone waits on, user imports, hot reload) run at HighQoS and normal priority and always go first. Background jobs (autosave, recovery, flushes, thumbnails, scans) run at EcoQoS. QoS switches per step. Durable jobs are never cancelled. Writes to one path run first in, first out.

**Wakes On:** A binary_semaphore released by main after it pushes to the SPSC request ring, with a 500 ms timeout that drives the hot-reload scan. A stop_callback releases it.

**Never Touches:** Any World or registry, SDL video, events or GPU, ImGui, the fork slot and the AssetRegistry tables. It receives and returns values. It may call wakeMain() when the editor idles.

**Why:** Decoding and parsing take milliseconds to tens of milliseconds. On main they would cause spikes; on a fork worker they would delay joins. One thread keeps write ordering trivial: the red team showed a second lane thread reordering saves on 12+ core machines.

### log worker

**Name:** log worker

**Count:** 1

**Owns:** The spdlog sinks (file and panel ring), used synchronously on this thread only. Per-call-site rate-limit summaries and dropped-record counts.

**Runs At:** When woken, and at least every 50 ms. Default QoS.

**Wakes On:** A binary_semaphore released by the first record pushed after it parks, with a 50 ms timeout.

**Never Touches:** World state, SDL, ImGui and the GPU. It never blocks a producer: a full ring drops the record and counts it.

**Why:** spdlog's async consumer holds its queue mutex while dequeuing. An EcoQoS worker preempted inside it would stall main (red team C15). Per-thread SPSC rings remove every lock, and the rings double as the crash-log tail captured in dumps (P12).

### watchdog

**Name:** watchdog

**Count:** 1, in every build

**Owns:** Hang detection, the shutdown deadline and dump requests. It reads only atomics: main's iterate counter; its Suspended, Modal, in-driver-wait and heartbeat flags; the breadcrumbs; and the InputFrame ring head.

**Runs At:** Once per second, on QueryUnbiasedInterruptTime or CLOCK_MONOTONIC, which exclude system sleep. Armed after the first SDL_AppIterate; until then only a 30 s boot budget applies. Joined last.

**Wakes On:** A binary_semaphore with a 1 s timeout; a stop_callback releases it.

**Never Touches:** World state, SDL, ImGui and the GPU. It makes no in-process DbgHelp calls, never DebugBreak, and never kills the process except at the shutdown deadline.

**Why:** One owner thread means a gameplay hang freezes the editor. A hang is declared after 5 consecutive 1 s periods with no progress, not Suspended, not Modal, no heartbeat and no debugger. A wait inside SDL_GPU acquire, present or fence calls is labelled a GPU or compositor stall. The watchdog then requests one out-of-process Crashpad dump per episode, carrying the registered streams and the Play-start bytes. At shutdown it enforces a 5 s deadline that starts after the Durable drain: dump request, log drain, then termination.

### input sampler

**Name:** input sampler

**Count:** 1

**Owns:** Gamepad polling. With SDL_HINT_AUTO_UPDATE_JOYSTICKS=0, it calls SDL_UpdateGamepads (documented safe from any thread). SDL then queues pad events stamped at about 1 ms resolution.

**Runs At:** 1000 Hz on AC power and 250 Hz on battery (assumed values; cost measured in se_bench). Parked while the editor idles or the app is Suspended.

**Wakes On:** SDL_DelayNS (a high-resolution waitable timer); stop is checked every period.

**Never Touches:** World state, InputService, ImGui and the GPU. Through SDL's queue it pushes only gamepad and joystick events.

**Why:** With SDL's defaults, XInput pads are polled once per presented frame. A tap between polls vanishes, chord order is lost, and every pad edge lands on the frame's last tick (red team P10).

### vblank sampler (Windows Paced mode)

**Name:** vblank sampler (Windows Paced mode)

**Count:** 1, only while Paced mode is active on Windows

**Owns:** Vblank timing of the output that shows the window. It blocks in IDXGIOutput::WaitForVBlank on that output (matched through MonitorFromWindow and EnumOutputs on every adapter) and publishes each vblank's QPC stamp in one 64-bit atomic.

**Runs At:** Once per refresh. HighQoS.

**Wakes On:** Each vblank of its output. Main publishes a new output id after DISPLAY_CHANGED or a mode change.

**Never Touches:** World state, SDL, ImGui, the swapchain and engine GPU objects.

**Why:** DwmGetCompositionTimingInfo(NULL) is desktop-wide, so on a second monitor or a mixed-refresh laptop it predicts the wrong vblank (red team P2). Main drops to Standard when the sampler's period and SDL's display mode disagree by more than 0.5%.

### library-owned threads

**Name:** library-owned threads

**Count:** miniaudio: 1 device thread ('library: miniaudio device thread') and 1 resource-manager thread. Tracy: workers only while a client is connected. SDL: the raw-input thread when SDL_HINT_WINDOWS_RAW_KEYBOARD is on in runtime builds ('library: SDL raw-input thread'), HID and joystick helpers, and a transient file-dialog thread per open dialog (on Windows always a separate, detached thread). GPU driver threads. The Crashpad handler is a separate process, not a thread.

**Owns:** Their own state only: the mixer, the profiler queue, raw-input timestamps, and a dialog's paths until its callback runs.

**Runs At:** Defined by each library. The audio period is about 10 ms (assumption).

**Wakes On:** Triggers internal to each library.

**Never Touches:** World state, registries, ImGui and engine GPU objects. The miniaudio end-of-sound callback writes ids into one SPSC ring sized for the voice cap. A file-dialog callback copies paths into its refcounted DialogMailbox, calls wakeMain() and returns. An immediate-dispatch event pushed off main is copied into the inbox by the event filter and dropped from SDL's queue.

**Why:** These libraries need their own threads. The engine shares with them only thread-safe APIs called from main, one SPSC ring, the inbox and dialog mailboxes. Some of these threads wait on main: an owned dialog sends messages to its owner window. So main never waits on them outside shutdown.

## Data Flow

### Keyboard, mouse, gamepad, text, window, display and quit events with SDL timestamps

**From:** SDL event queue (window thread's Win32 queue; SDL raw-input thread; input sampler's gamepad updates)

**To:** main: InputService, ImGui, window state

**What:** Keyboard, mouse, gamepad, text, window, display and quit events with SDL timestamps

**Mechanism:** SDL pumps between iterates and calls SDL_AppEvent on main. In the editor ImGui sees each event first; its capture flags and viewport focus gate game input.

**Frequency:** Every event, pumped once per presented frame

**Ownership Rule:** Main only. No system reads SDL input state.

### TERMINATING, LOW_MEMORY and the four background/foreground events

**From:** Any thread pushing an immediate-dispatch event off main (SDL internals; SDL's Windows desktop backend does not send these)

**To:** main: event inbox

**What:** TERMINATING, LOW_MEMORY and the four background/foreground events

**Mechanism:** An SDL event filter runs on the pushing thread before SDL's watchers. Off main it copies these events into a mutex-guarded vector and returns false, so SDL never drains its queue on that thread. At iterate start main swaps the vector with try_lock and dispatches with no lock held.

**Frequency:** Rare; expected never on Windows desktop; counted

**Ownership Rule:** Lock order is SDL's watch lock, then the inbox lock, never the reverse. Main never calls SDL under the inbox lock.

### InputFrame: tick, held bits, axes, and ordered edges with timestamps and sources

**From:** main: InputService

**To:** main: Schedule.step of every running World

**What:** InputFrame: tick, held bits, axes, and ordered edges with timestamps and sources

**Mechanism:** latch(tick, windowEndNs, lastTickOfFrame) returns the frame by value. Each edge goes to the tick whose window holds its stamp. The last tick takes later edges; in a frame with no tick, edges wait for the next tick (up to 6.9 ms at 144 Hz). Each frame is copied into the replay ring.

**Frequency:** Per tick (120 per second)

**Ownership Rule:** Value semantics.

### Play-World edits, generator spawns, asset swaps with content hash, GameState commands, Pause/Step/Stop

**From:** main: editor UI, stress generator, hot reload, GameState requests

**To:** main: play World and GameStateStack, through the SessionLog

**What:** Play-World edits, generator spawns, asset swaps with content hash, GameState commands, Pause/Step/Stop

**Mechanism:** Each is queued with the index of the tick it precedes. SimDriver::tickInline applies it at that tick boundary and recomputes the running Worlds before every tick. While paused it applies at once and is logged against the next tick.

**Frequency:** Per action

**Ownership Rule:** The SessionLog travels with every replay and bundle.

### EditorCommands

**From:** main: editor UI

**To:** main: document World

**What:** EditorCommands

**Mechanism:** Document edits apply after ImGui::Render and before the RenderList build. Resource-releasing commands (asset delete or reimport, document close, viewport resize) apply at the next frame start.

**Frequency:** Per user action; a drag coalesces into one command

**Ownership Rule:** Main only. The undo stack owns the before/after JSON.

### A descriptor (invoke, ctx, count, grain, chunks, batch), read spans over pre-created storages, per-chunk outputs

**From:** main (fork)

**To:** fork-join workers

**What:** A descriptor (invoke, ctx, count, grain, chunks, batch), read spans over pre-created storages, per-chunk outputs

**Mechanism:** Main writes the descriptor with plain stores, sets remaining = chunks, then does m_claim.store(pack(gen, chunks, 0), release). It wakes participants - 1 workers, each with its own wake word and notify_one. Workers claim with fetch_add(batch, acq_rel) and copy the descriptor once per generation.

**Frequency:** Per enabled fork site: at most 8 per tick and 4 per frame

**Ownership Rule:** Borrowed for the fork only. A chunk writes only its range or its own output, and touches only the site's declared component sets. No structural calls.

### Per-chunk outputs, the first exception, timing

**From:** fork-join workers

**To:** main

**What:** Per-chunk outputs, the first exception, timing

**Mechanism:** Writes, then remaining.fetch_sub(1, acq_rel) as the last access. Main waits until remaining.load(acquire) == 0, then concatenates outputs in chunk order.

**Frequency:** Per fork

**Ownership Rule:** Main owns all outputs after the join. Results do not depend on which thread ran a chunk.

### AssetRequest, SaveRequest, recovery, autosave and flight-recorder bytes, replay and SessionLog chunks

**From:** main

**To:** background lane

**What:** AssetRequest, SaveRequest, recovery, autosave and flight-recorder bytes, replay and SessionLog chunks

**Mechanism:** An SPSC request ring, then a binary_semaphore release. JobDesc carries the class (Urgent or Background), the durable flag, the strand (destination path) and the revision.

**Frequency:** On demand

**Ownership Rule:** Requests are values; no reference into engine state.

### Decoded payloads, or Error{code, message, path, line, column}

**From:** background lane

**To:** main: AssetRegistry and upload queue

**What:** Decoded payloads, or Error{code, message, path, line, column}

**Mechanism:** An SPSC completion ring, drained once per frame at iterate start. Payloads go to the upload queue.

**Frequency:** Once per frame

**Ownership Rule:** Payloads move to main. Completions for a released scope are dropped.

### Texture and buffer uploads

**From:** main

**To:** GPU (uploads)

**What:** Texture and buffer uploads

**Mechanism:** A dedicated upload command buffer, submitted on every iterate with pending uploads, presenting or not, within a byte budget (8 MB per frame assumed). When its fence signals, the texture flips generation and the old one goes to the retire queue. CPU bytes are kept until then; DeviceLost re-reads cooked files.

**Frequency:** Per iterate with uploads

**Ownership Rule:** A texture is used only once resident.

### Vertices, indices, ImGui vertices

**From:** main and enabled RenderList chunks

**To:** GPU (frame)

**What:** Vertices, indices, ImGui vertices

**Mechanism:** The transfer buffer is mapped (cycle=true) before forks; chunks write disjoint ranges. The FrameRecorder records noexcept in one command buffer, submits with a fence, then runs the retire queue.

**Frequency:** Per frame

**Ownership Rule:** Every SDL_GPU call happens on main and is asserted. ImTextureIDs are resolved from handles this frame and never stored.

### The swapchain image

**From:** main

**To:** display

**What:** The swapchain image

**Mechanism:** VSYNC in Paced and Standard; IMMEDIATE with ALLOW_TEARING in VRR. A frame-latency waitable with SetMaximumFrameLatency(1 or 2) and present statistics come from the SDL patch. A NULL acquire skips the present, and the command buffer is still submitted.

**Frequency:** Per frame, or every second vblank in HalfRate

**Ownership Rule:** Only the acquiring command buffer references the image.

### QPC stamp of each vblank of the window's output

**From:** vblank sampler

**To:** main

**What:** QPC stamp of each vblank of the window's output

**Mechanism:** One 64-bit atomic store (release) and load (acquire). Main writes the output id.

**Frequency:** Per refresh

**Ownership Rule:** The sampler only writes the stamp.

### Voice play, stop, volume and pan

**From:** main

**To:** miniaudio

**What:** Voice play, stop, volume and pan

**Mechanism:** Direct ma_sound calls after each tick, with start = max(now, tick anchor + D) through ma_sound_set_start_time_in_pcm_frames. Each voice record holds its PCM by shared_ptr. Releasing a scope stops and uninitialises its voices first.

**Frequency:** Per tick

**Ownership Rule:** Main is the only caller of ma_engine and ma_sound.

### Ids of finished sounds

**From:** miniaudio device thread

**To:** main

**What:** Ids of finished sounds

**Mechanism:** An SPSC ring sized for the voice cap (256). The producer writes, then tail.store(release). Main also sweeps voices where ma_sound_at_end is true, calls ma_sound_uninit, then drops the PCM reference.

**Frequency:** Drained every iterate, including at 10 Hz while Suspended

**Ownership Rule:** The callback is noexcept, never allocates and never waits.

### Chosen paths

**From:** SDL file-dialog thread

**To:** main

**What:** Chosen paths

**Mechanism:** A refcounted DialogMailbox (the callback holds its own reference, never an Editor*), then wakeMain(). Main drains it at iterate start with try_lock.

**Frequency:** Per dialog

**Ownership Rule:** The callback touches only its mailbox.

### Iterate counter; Suspended, Modal, in-driver and heartbeat flags; breadcrumbs; InputFrame ring head

**From:** main

**To:** watchdog

**What:** Iterate counter; Suspended, Modal, in-driver and heartbeat flags; breadcrumbs; InputFrame ring head

**Mechanism:** Relaxed counter, release stores on the flags, packed 64-bit breadcrumbs, seqlock-stamped ring slots

**Frequency:** Continuous

**Ownership Rule:** The watchdog only reads.

### Log records carrying the sim tick and thread role

**From:** all engine threads

**To:** log worker, then the file sink and main's Log panel

**What:** Log records carrying the sim tick and thread role

**Mechanism:** One SPSC ring of fixed 256 B records per thread. A full ring drops and counts. Panel lines return to main through an SPSC ring, formatted once.

**Frequency:** Per log call

**Ownership Rule:** Nothing blocks a producer. The rings are also dump streams.

### Breadcrumbs, log rings, InputFrame ring, SessionLog tail, flight recorder, system info; the Play-start bytes attached

**From:** all engine threads

**To:** Crashpad handler (separate process)

**What:** Breadcrumbs, log rings, InputFrame ring, SessionLog tail, flight recorder, system info; the Play-start bytes attached

**Mechanism:** Memory registered at boot and read out of process at dump time

**Frequency:** Per crash or hang

**Ownership Rule:** The engine process never writes a dump itself.

### A const view of the World

**From:** main (World)

**To:** main (RenderList builder, editor panels)

**What:** A const view of the World

**Mechanism:** WorldRenderSource wraps the const World& and alpha. Faulted Worlds pick by brute force over storages.

**Frequency:** Per frame

**Ownership Rule:** No copies and no locks. The reader runs after the frame's last tick.

## Goals

### cpu-utilization

**Goal:** cpu-utilization

**Mechanism:** A deterministic fork-join pool, one worker per physical core (E-cores for claim-based sites), runs enabled fork sites. A background lane takes I/O. The samplers and the log worker are tiny. Nothing spins.

**Expected:** At the targets the loop used 5.9-23% of one core (measured), so W = 0 ships. Where a site is enabled, the tick ran 3.0-3.6x faster (measured: 0.398 to 0.134 ms at N=5k, 4.20 to 1.18 ms at N=50k with 6 workers). Serial simulation and serial rendering never overlap.

**How To Verify:** se_bench per fork site: parallel against inline, overhead at most 10%, and a 10-minute tail soak on the weakest machine. Busy percentages from Tracy and QueryThreadCycleTime.

**Verdict:** partly-met

### frame-latency

**Goal:** frame-latency

**Mechanism:** Physics stays on the frame's critical path. Mitigations:
- the budget-sized cap and the Overloaded state;
- Locked cadence and next-frame planning;
- forks;
- spike sources off the tick;
- the frame-latency waitable and present statistics;
- a per-output vblank sampler;
- VRR mode;
- HalfRate hysteresis;
- adaptive frame latency.

**Expected:** Measured: 0 hitches without spikes up to N=50k at 59.9 Hz and N=10k at 144 Hz. A 25 ms tick costs one missed vblank (2 hitches per run), while threaded designs kept cadence but froze the world too. Computed: the cap gives 1 hitch per spike at N=50k instead of the 30 Hz lock-in. The waitable removes the one-refresh-late equilibrium the red team inferred. Real-stack latency is unmeasured.

**How To Verify:** se_bench spike scenes; the 60 s 59.94 Hz scene; PresentMon displayed time plus a photodiode; a two-monitor case; an injected late frame must drain within 2 frames.

**Verdict:** not-met

### independent-rates

**Goal:** independent-rates

**Mechanism:** A fixed step in seconds (dt = 1/120 s, configurable). Locked or Free cadence and the budget cap. Interpolation in Free cadence. Timestamp placement of keys, and pads sampled at 1 kHz. Audio at tick anchors. GameState changes and SessionLog entries at tick boundaries.

**Expected:** 2 ticks per frame at 60 Hz (Locked; 0.1% dilation at 59.94 Hz), 5 per 6 at 144 Hz, 8 per 11 at 165 Hz. Results depend only on the tick count, the InputFrames and the SessionLog. Wall-clock cadence is not independent: ticks bunch at frame start and stop while main blocks.

**How To Verify:** CI replays the same input, including a GameOver transition and logged editor edits, under synthetic 30/60/144/160 Hz clocks and checks identical hashes. Plus a placement test and a ticks-per-second check over 10 s.

**Verdict:** partly-met

### scalability

**Goal:** scalability

**Mechanism:** Data-parallel phases go to fork sites. Serial rules stay on main, and heavy serial code is rewritten as gather-compute-scatter. The Schedule panel shows serial and parallel time per phase.

**Expected:** Computed from measured per-entity costs, the serial loop holds 59.9 Hz up to about N=85k. At 144 Hz with one tick per frame, the red team measured 0 hitches at N=35k and 40k and 5-17 at 50k. With 6-worker forks, a 50k tick p99 of 1.62 ms plus render p99 of 1.76 ms is 3.4 of 6.94 ms. The serial counting sort is about 35% of the 6-worker tick.

**How To Verify:** se_bench sweeps N on the 7700X, a 4-core laptop and a 2P+8E laptop, with CI budgets per serial phase.

**Verdict:** partly-met

### isolation

**Goal:** isolation

**Mechanism:** Contracts and the link graph: systems take World or ParallelView, the renderer takes a const RenderSource, and banned-include checks keep SDL and ImGui out of se_core. Each World has its own lifecycle. Exceptions are contained in Play sessions and in the frame transaction. Quit is durable. Dumps are written out of process.

**Expected:** Today's write from the render pass (app/scene/Sandbox.cpp:155) would no longer compile. One thread runs both halves, so a hang or breakpoint stops both. Durable saves, recovery files, idle autosave and out-of-process hang dumps cover that. Debug editor responsiveness depends on the Dev preset.

**How To Verify:** Compile-fail test for a render pass that writes the World. Tick and render fault injection: the editor stays alive and the document hash is unchanged. Hang injection: a dump within 6 s. Quit with dirty documents on a throttled disk.

**Verdict:** partly-met

## Issues

### sync-overhead

**Issue:** sync-overhead

**What Goes Wrong:** Forking small loops costs more than it saves. Measured fork overhead was 5-15 us p50 back to back and 17-25 us p50 with parked workers. Integrate+bin took 18.9 us in parallel against 11.3 us serial at N=5k. Waking every worker costs battery. A shared cache line between remaining and the descriptor adds misses (P17). A mutex shared with an EcoQoS thread inverts priority (C15).

**Design:** No world lock and no snapshot. Forks only at enabled sites. Chunks of at least 10 us, at least 4 chunks per participant, participants = min(W+1, chunks/4). Fused phases; at most 8 forks per tick and 4 per frame. Per-worker notify_one. remaining has its own cache line; the descriptor is copied to locals once per generation; large sites claim in batches. Spin at most 20 us, only when the next fork is predicted. Lane, log and audio queues are SPSC rings, and main uses try_lock on the inbox and mailboxes.

**Enforcement:** ForkStats per site. se_bench fails a fork slower than inline or with overhead above 10%. A debug assert fires on a fork below threshold. Tracy zones per fork. A review rule lists every lock main can touch.

**Residual Risk:** Unexplained 12-29 ms wake stalls appeared in 5 of 29 pool runs; trace them with ETW/WPR before any site is enabled by default. On 2-4 core laptops W may stay 0.

### races

**Issue:** races

**What Goes Wrong:** Ways a race can get in:
- A chunk creates a pool lazily through emplace or a non-const view (dep/entt-3.16.0/src/entt/entity/registry.hpp:257-275), writes outside its range, pushes into a shared sink, reads a component another chunk writes, or calls ImGui.
- A lane job keeps a pointer into main's tables.
- SDL's watcher drains the queue on a pushing thread.
- An SDL file-dialog callback runs on SDL's dialog thread (on Windows always).
- A cached ImTextureID outlives its texture container, which SDL's D3D12 backend frees at release.

**Design:** The World is the only door. Structural operations run on main and assert not-in-fork. Storages are pre-created. ParallelView enforces range and declared read/write sets. Per-chunk outputs and sinks. ENTT_USE_ATOMIC; no on_update listeners; GImGui thread_local. The lane passes values. An event filter diverts off-main immediate events into the inbox. Dialog callbacks only fill a refcounted mailbox. Texture IDs are resolved per frame, and releases run after submit.

**Enforcement:** Clang -Werror=thread-safety with MainThreadRole. SE_ASSERT_MAIN_THREAD and SE_ASSERT_NOT_IN_FORK. Linux TSan with chaos delays and pile-up scenes. Banned-include check. Inbox and foreign-thread dialog tests. A grep check bans ImTextureID or SDL_GPUTexture* members in se_editor. A debug RenderDrawData wrapper asserts that no bound texture was released.

**Residual Risk:** Low. A raw pointer smuggled into a chunk can still alias. Windows-only paths rely on asserts, because TSan runs on Linux only.

### debugging

**Issue:** debugging

**What Goes Wrong:** Bugs that appear only with workers or in one tick pattern (the 0-tick frame at 144 Hz). Catch-up storms after breakpoints. Hangs with no trace. Replays that diverge because editor edits, hot reloads, spawns or GameState changes landed at wall-clock-dependent ticks (C6, C7). Log tails lost at a crash (P12). Field replays that diverge on CPUs without FMA3 (P13). Colliding in-process dump writers (C11). Field stutter reported with no data (P15).

**Design:** SE_JOBS=0 is bit-identical (measured). Also: SE_BG_INLINE, --det-crosscheck, --fixed-frame-dt, a chaos executor, replays plus the SessionLog. Breadcrumbs {role, phase, system, tick, chunk:16}. The sim tick on every log line and Tracy zone. Debugger stalls count as paused time. Faulted Worlds are kept. Out-of-process dumps with registered streams. Deterministic trig and _set_FMA3_enable(0). A pacing flight recorder. A Dev preset.

**Enforcement:** CI golden replays with W of 0, 1, 3 and 7 and 8 chaos seeds. Replays with a GameOver transition and logged edits under 30-160 Hz clocks. FMA3 forced on and off. Crash tests check that breadcrumbs and log rings appear in the dump.

**Residual Risk:** GPU, driver and live-input timing stay nondeterministic. The simulation reproduces exactly from the InputFrames plus the SessionLog. Golden hashes are per platform and compiler.

### inconsistent-snapshots

**Issue:** inconsistent-snapshots

**What Goes Wrong:** No cross-thread snapshot exists, so only visual problems remain:
- a teleport drawn as a streak;
- a spawn sliding from the origin, including on_construct firing before Transform exists (C17);
- a despawn lingering;
- the inspector disagreeing with the viewport;
- a scene switch interpolated between worlds;
- a faulted World shown in mid-tick;
- pause and resume jumps;
- attached children streaking after a parent teleports;
- 1-tick visual events never shown at 60 Hz (P16).

**Design:** Reads happen on main after the last tick. Locked cadence renders at alpha = 1. Free cadence uses PrevTransform on the entity. A spawn-finalize pass in Cleanup sets prev = current and teleportTick = spawnTick; there is no on_construct hook, and Transform is deserialized first. Teleport marks propagate through Attachment. Pause sets prev := current once. Paused, stepping, edited and faulted Worlds render at alpha = 1. A per-frame visual-event list, plus an importer rule that discrete states last at least 2 ticks. Derived structures carry a validTick.

**Enforcement:** Unit tests at alpha 0.5 for spawn (both component orders), despawn, teleport, cut, attachment teleport, id reuse, and pause/resume. A debug overlay for unmarked jumps. An importer warning for 1-tick states. A 60 Hz preview toggle.

**Residual Risk:** Visual only: at most one tick of disagreement during Free-cadence play. A faulted World can show a half-applied tick, and it is labelled as such.

### lifecycle-shutdown

**Issue:** lifecycle-shutdown

**What Goes Wrong:** Things that can break at quit and at the edges of the frame:
- The quit prompt is never drawn, because SDL stops iterating once a callback returns SUCCESS, and queued saves are cancelled (C1).
- GPU objects are released while ImGui draw data still names them (C2).
- A NULL acquire is mistaken for suspension (C3).
- A render exception leaks an acquired swapchain image (C5).
- The audio thread reads freed PCM (C9).
- Uploads are skipped while minimized (C10).
- SDL_AppQuit runs after a failed init (C12).
- Teardown hangs after the watchdog has stopped (C11).
- Device loss mid-frame.

**Design:** Forks are synchronous. Quit is two-phase with Durable saves; the deadline starts after the drain. An 18-step shutdown order on main. NULL acquire is a skipped present. A frame transaction guard. A retire queue. Voice refcounts. An upload queue independent of presenting. noexcept callbacks; SDL_AppQuit accepts NULL. The watchdog stays alive to the end, SDL subsystems are quit explicitly, and release builds terminate after the final flush.

**Enforcement:** Quit at a random frame 100 times under ASan/UBSan and TSan. Quit with N dirty documents on a throttled disk. NULL-acquire injection. Throw injection at every recording step. AppInit failure at every step. Audio Stop and hot-reload lifetime tests. A minimized-upload test.

**Residual Risk:** A driver can hang in device destruction; the deadline and termination bound it. A single editor stb_image call cannot be split (tens of ms, estimated). A disk that never finishes a Durable save keeps the quit modal open until the user cancels.

### bottleneck-idle

**Issue:** bottleneck-idle

**What Goes Wrong:** Main is the bottleneck by design: serial rules, ImGui and submission grow until k*S + R > T while other cores sleep. At frame latency 1, CPU and GPU never overlap (P6). Debug editor builds tick 3-111x slower (P9). An idle editor burns power (P19). On 2P+8E laptops the pool would use 2 of 10 cores (P8).

**Design:** Fork sites, with E-cores for claim-based work. Gather-compute-scatter. The Schedule panel. The budget cap and Locked cadence. HalfRate hysteresis. Adaptive frame latency 1-2. The Dev preset and an editor UI floor (Overloaded allows 0 ticks in editor Play). Idle waitevent. Spike sources off the tick. The escalation seam.

**Enforcement:** se_bench phase budgets on 7700X, 4-core and 2P+8E profiles, in Release and Dev. Editor frame at 144 Hz at most 6.4 ms (Release). Game frame at most 50% of a 144 Hz period. Overload counter 0 at the targets.

**Residual Risk:** The real loss zone is (T - R - m)/k < S <= dt - 0.5 ms (computed: 4.4-7.8 ms for a 144 Hz game, 2.4-7.8 ms for the editor, 7.1-7.8 ms at 60 Hz). Measured target ticks are 0.4-1.6 ms, and 77-94% of each frame is idle. An /Od editor dilates the simulation at the targets.

## Snapshot And Interpolation

There is no cross-thread snapshot. The renderer and the editor read the World on main, after the frame's last tick, through a const reference.

Cadence decides whether interpolation is needed
- Locked cadence applies when the presentation period is within 0.5% of k*dt or dt*k: 60, 59.94 and 120 Hz, and VRR mode. Each frame runs exactly k ticks per elapsed vblank, so the World after the last tick is that frame's state. It renders at alpha = 1, with no interpolation and no added lag. At 59.94 Hz the simulation runs 0.1% slow in wall time, counted as dilation.
- Free cadence applies at 144 Hz, 165 Hz and other non-integer ratios: alpha = accumulator/dt after stepping, and the renderer interpolates.

Interpolation state (Free cadence)
- PrevTransform {position, rotation, scale, tick} sits on every Interpolated entity, plus the previous Camera2D transform: about 28-32 B per entity, 0.3 MB at 10k (computed).
- Movement copies Transform into PrevTransform at the start of each tick: about 30 us serial at 10k (estimated), fused into the integration fork when it is enabled.
- Position and scale lerp; rotation takes the shortest arc. The camera uses the same alpha and parallax follows the camera. Screen-space UI is not interpolated.
- Rows are never paired across two states, because prev lives on the entity.

Discrete state
- Animation frame, tint, hit flash, visibility, layer, z and flip come from the current tick.
- At 60 Hz the first tick of each frame is never shown, so a 1-tick discrete state would appear with probability 1/2 (computed; red team P16). The fixes:
  - The importer requires discrete visual states to last at least ceil(tick rate / lowest display rate) = 2 ticks.
  - Transient visual events (hit flash, parry flash, first active frame) go into a per-frame visual-event list that the RenderList consumes, so each shows for at least one presented frame.
  - The editor has a 60 Hz preview toggle.

Edge cases
- Spawn: no on_construct hook, because EnTT fires it before the entity's later components exist. A spawn-finalize pass in Cleanup sets prev = current and teleportTick = spawnTick for every entity created in the tick; World loading runs the same pass. Deserialization emplaces Transform first.
- Despawn: DestroyRequests drain in Cleanup, so the entity is not drawn.
- Teleport: Transform::teleport records teleportTick, and teleportTick == current is drawn as prev = current. Camera2D::cut does the same. Marks propagate to children through the one-level Attachment.
- Id reuse: nothing is matched by id, so EnTT's 12-bit version wrap (dep/entt-3.16.0/src/entt/entity/entity.hpp:38-39) cannot pair two entities.
- Pause sets prev := current once, so there is no backward snap on resume. Paused, stepping, edited and faulted Worlds render at alpha = 1, matching the inspector.
- Scene change: new Worlds are fully built, and an outgoing World still on screen renders at alpha = 1.
- Faulted World: derived structures carry a validTick set when their build completes, and only valid ones are used.
- Dilation keeps the accumulator's fractional part. A sim-rate change applies between ticks.

The seam
- The RenderList builder reads only a RenderSource: SpriteView ranges (pos, prevPos, rot, prevRot, scale, texture handle, region, sort key, tint, flags), visual events, cameras, tile chunks, parallax, text and debug primitives. Today WorldRenderSource wraps the const World and alpha.
- If a flip condition is ever measured, a SnapshotRenderSource backed by the dedicated-threads SnapshotChannel plugs in. That channel uses SPSC index rings, release publish and acquire receive, one owner per slot, and keys rather than pointers.
- Never reuse the render-optimization triple buffer. Its load and store are separate (render-optimization:core/util/RenderCommands2D.hpp:80-81), and it tore 3 of 406 frames under an injected 9-16 ms reader stall, against 0 for a single-exchange buffer (measured).
- Escalation would cost (measured): extraction 6.4-66 us per tick for 112 KB to 1.1 MB, publish 10-20 ns p50, and no slowdown rendering from the cross-core copy (70.2 vs 71.2 us at 10k).

## Synchronization

One thread owns world state, so no mutex, shared_mutex or atomic protects it. These are all the cross-thread mechanisms.

1. Fork-join, only when a site is enabled. Each of these sits on its own 64-byte line:
   - a read-mostly descriptor {invoke, ctx, count, grain, chunks, batch};
   - remaining;
   - failed plus an exception_ptr;
   - the claim word [gen:16 | chunks:24 | next:24];
   - one wake word per worker.
   - Publish: main writes the descriptor with plain stores and sets remaining = chunks (relaxed). It then does m_claim.store(pack(gen, chunks, 0), release). For participants - 1 workers it does wake[i].fetch_add(1, release) and notify_one.
   - Worker loop: old = wake[i].load(acquire); try to claim; wait(old). A bump between the load and the wait returns at once, so no wake is lost. A wake after the fork has ended counts as late in ForkStats.
   - Claim: w = m_claim.fetch_add(batch, acq_rel). Chunks next(w) to next(w)+batch-1 below chunks(w) belong to fork gen(w). The RMW reads from the release sequence of main's store, so the descriptor is visible. Each participant copies it once per generation.
   - Main never republishes while remaining > 0, and gen, chunks and next share one word, so a claimer always reads its own fork's descriptor. This fixed the prototype's stale-worker bug.
   - Complete: writes, then remaining.fetch_sub(1, acq_rel) as the last access. The participant that reaches 0 calls notify_one. Contrast render-optimization:core/util/TaskGroup.hpp:49-56, which decrements and then touches the group.
   - Join: main claims chunks like any worker, then waits while remaining.load(acquire) != 0.
   - Exceptions: the first failing chunk does failed.exchange(true, acq_rel) and stores the exception_ptr. Later chunks skip their work but still decrement. Main rethrows after the join. There is no moved-from fallback (render-optimization:core/util/TaskGroup.hpp:29-38).
   - Nested forks run inline.
2. Background lane: an SPSC request ring and an SPSC completion ring, plus a binary_semaphore that main releases. The lane waits on it with a 500 ms timeout. No mutex is shared with main.
3. Log rings: one SPSC ring of fixed 256 B records per engine thread. Producers release the log worker's semaphore only when it is parked; a full ring drops and counts. Panel lines return to main through one more SPSC ring.
4. Inbox: the SDL event filter runs under SDL's event-watch lock on the pushing thread and appends to a mutex-guarded vector. Main swaps it with try_lock at iterate start and dispatches with no lock held. It never calls SDL under that lock.
5. File-dialog mailboxes: a refcounted {mutex, paths, done}. The callback fills it, calls wakeMain() and drops its reference. Main drains with try_lock.
6. miniaudio: an SPSC end-of-sound ring sized for the voice cap (256), with release/acquire head and tail. The callback never waits.
7. Watchdog: relaxed loads of the counter and the packed breadcrumbs; release/acquire flags; seqlock-stamped InputFrame slots and a published head.
8. Vblank sampler: one 64-bit atomic QPC stamp (release/acquire) and an output id written by main.
9. Input sampler: nothing of its own. SDL_UpdateGamepads queues events through SDL's thread-safe queue.
10. wakeMain(): SDL_PushEvent of one user-event type, used by the lane and dialog callbacks. It is never one of SDL's six immediate-dispatch types.
11. Crash capture: nothing in process. The Crashpad handler reads registered memory from its own process.
12. Pacing: the frame-latency waitable (SDL patch); SDL_DelayNS, a high-resolution waitable timer with no timeBeginPeriod (today SFML toggles it per frame: dep/SFML-3.0.2/src/SFML/System/Win32/SleepImpl.cpp:47-54); SDL_WaitForGPUSwapchain; SDL_QueryGPUFence and SDL_WaitForGPUFences for the fence N-L check.

Priority rule: main never takes a lock that an EcoQoS or below-normal thread can hold, except through try_lock.

Cost: at the targets with W = 0, each frame does one completion-ring drain, one inbox flag load and a few log pushes. With forks, measured cost was 5-15 us p50 back to back and 17-25 us p50 with parked workers (selftest: 50 us chunks finished in 55-65 and 67-75 us). That is why chunks must be at least 10 us with 4 or more per participant.

Waits-for graph (acyclic)
- Main waits on fork joins, on the GPU (waitable, fences, swapchain) and, only at shutdown, on bounded joins and library teardown.
- Workers, the lane, the log worker, the samplers and the watchdog wait only on their own wake words, semaphores, timers or vblanks.
- No engine thread waits on main. Library threads can (an owned file dialog sends messages to its owner window; DXGI may send messages to the window thread), so main never waits on them outside shutdown. That keeps the DXGI rule.

## Job System

The pool
- A first-party ForkJoinPool of about 400 lines of std C++ (jthread, stop_token, atomic wait/notify), with enkiTS behind the same API as a fallback.
- Sizing comes from GetSystemCpuSetInformation (EfficiencyClass, CoreIndex, LastLevelCacheIndex) on Windows and sysfs on Linux, one worker per physical core:
  - W_max = physical cores - 2, capped at 15, or - 1 on homogeneous CPUs with 8+ cores: 7 on the 7700X, 2 on a 4-core laptop, 8 on 2P+8E, 12 on 6P+8E.
  - AnyPhysical sites (claim-based: narrowphase, culling, vertex generation) may use every worker, including E-cores and second-CCX cores. Claimed chunks tolerate unequal cores, and the join waits at most one chunk on a slow core.
  - PerformanceOnly sites (barrier-heavy, such as Box2D) use the P-cores on main's last-level cache minus 1.
  - Each EfficiencyClass is calibrated at startup with a 10 ms dummy fork.
- Main and the workers run HighQoS with SetThreadSelectedCpuSets soft affinity; main stays on a P-core.

Fork sites and enabling
- A ForkSite is {name, grain, minItems, core class, read set, write set, enabled, inline-until, stats}, registered by its system.
- parallelFor runs the same fixed chunks inline when the site is disabled, inside a fallback window, below minItems, or at W = 0, so results are identical.
- Sites ship disabled. One is enabled only when all of these hold:
  1. se_bench on the real stack records a miss;
  2. the TSan chaos suite, including pile-up scenes, is green;
  3. median parallel time beats inline, with overhead at most 10%;
  4. a 10-minute soak with background load on the weakest machine shows join p99.9 and max no worse than inline, and no missed wakes.
  Medians hide tails: 4-worker forked ticks peaked at 29.2 ms at N=20k (p50 0.53 ms) while the single loop's maximum was 2.26 ms (rig).
- Fallback: a join slower than 4x its median runs the site inline for 5 s, then retries. This is counted.
- With no site enabled, no worker threads exist. At the targets nothing qualifies. The first expected candidate is the Game of Life row bands, if that demo is ported: a judge-measured replica took 6.5 ms p50 and 10.1 ms max serially, and 1.32 ms with 7 workers.

Rules inside a fork
- Chunk boundaries depend only on (count, grain). Each chunk writes only its range or its own output.
- Variable outputs grow per chunk from per-chunk arenas (or count-then-fill where a contiguous span is needed) and are concatenated in chunk order.
- DebugDraw and other ctx sinks get per-chunk slices; World-level sinks assert not-in-fork.
- Declared read and write sets: in debug builds ParallelView rejects undeclared access, and reads of a written type outside the chunk's range.
- Merges run in chunk order. No floating-point atomics. Counter-based RNG of (seed, tick, Guid, stream).
- No structural calls, signals, dispatcher calls or ImGui. Nested forks run inline.
- Measured: identical hashes for 0, 2, 4 and 6 workers at N=10k and 50k.

Thresholds
- Chunks of at least 10 us (grain from measured per-item cost). participants = min(W+1, chunks/4), and only participants - 1 workers are woken. A fork needs about 80 us of work to use 2 participants.
- Fused phases; at most 8 forks per tick and 4 per frame; spin at most 20 us only when the next fork is predicted, never on battery.
- The descriptor and remaining sit on separate lines; large sites claim in batches.
- Why: integrate+bin was slower in parallel at N=5k (18.9 against 11.3 us), while the narrowphase scaled 6.3x at N=50k (3426 to 542 us), after which the serial counting sort was about 35% of the tick. The rig used a different protocol, so fork costs are re-measured with the real pool.

Candidate sites (grain in parentheses)
- Movement prev-copy fused with integration (1024).
- Swept AABB against tiles (256).
- World AABB plus cell histogram (1024).
- Scatter (1024).
- Body pushes, owner-computes per entity range (1024).
- Gameplay contacts per cell range (32 cells).
- Contact radix sort (4096).
- Paths and animators (1024).
- RenderList cull and keys, key sort, and vertices (1024).

Serial by rule: effects, gameplay rules, structural changes, dispatcher drains, GameState changes, SessionLog application, audio, ImGui, and GPU recording and submission. Heavy serial code takes the gather-compute-scatter shape of render-optimization:core/system/WayPointSystem.cpp:100-191.

Chaos mode (SE_CHAOS_SEED): a seeded claim permutation, yields, delays between claim and descriptor read and between last write and decrement, and injected late wakes. It runs under Linux TSan with the W = 0/1/3/7 hash comparison.

Box2D (conditional): a TaskSet on the same workers runs its enqueued tasks concurrently with the caller, using a bounded table and per-task latches, independent of the fork slot.

Catch-up, cadence and overload (replaces the 8-step cap)
- Inputs:
  - T: the presentation period (2T in HalfRate).
  - S, R, g: tick, non-tick CPU and GPU time (GPU from the SDL patch's timestamps). The _hi values are p99.5 over 4 s; the _95 values are p95 over 2 s.
  - m = max(1.5 ms, timer p99 + 0.5 ms).
- Cadence: Locked when T/dt or dt/T is within 0.5% of an integer (k ticks per vblank, alpha = 1, residue counted as dilation); otherwise Free (accumulator; frame delta from present statistics, or snapped within max(1.0 ms, timer p99 + 0.1 ms)).
- k_rt = ceil(T/dt): 2 at 60 Hz, 1 at 120, 144 and 160 Hz.
- k_hi = floor((T - R_hi - g_hi - m)/S_hi); k_95 is the same with p95 costs.
- Normal: k_max = clamp(max(k_rt, k_hi), 1, min(8, k_rt + 1)).
- Overloaded: clamp(k_95, 1, k_rt) in game builds; clamp(k_95, 0, k_rt) in editor Play, so the UI stays at 60 Hz or more.
- If GPU time is unknown and the previous fence had not signalled at frame start, k_max = k_rt.
- steps = min(due, k_max).
- Planning: before sleeping, the pacer computes the next frame's steps and wakes at the predicted vblank - B(steps), where B = min(T, steps*S_hi + R_hi + g_hi + m). A miss adds 1 ms to B, decaying 0.05 ms per frame. Misses while Modal do not count.
- Debt that the spare rate (k_max - T/dt) cannot repay within 0.25 s becomes dilation: counted, re-based keeping the fraction, shown as a badge, sent to Tracy, and logged once per episode with the 3 costliest systems. No tick is ever skipped.
- Overloaded: enter after 2 consecutive missed vblanks (present statistics) with debt, or k_95 < k_rt for 1 s; leave after 1 s with no misses and k_95 >= k_rt.
- Paused time: stalls over 0.25 s (100 ms with a debugger), Suspended time and system sleep.
- HalfRate above 100 Hz: enter at more than 1% misses over 2 s; leave after 5 s of p95(C+g) + m below 0.75*T.

Why the old cap had to go, and what the red team changed
- At N=50k with a 25 ms spike, the 8-step loop owed 4 ticks per frame (4 x 4.2 + 1.4 = 18.2 ms > 16.69 ms). It locked at 30 Hz with 115 hitches (measured). A 3-step cap gave 2 hitches (measured).
- Here k_max = 2 (S_hi about 5.0 ms), which gives 1 missed vblank per spike (computed).
- A fixed late latch at 59.94 Hz and N=30k locked the same way: 969 hitches in 40 s; one more tick of headroom gave 0 (red team, measured). Locked cadence removes the 3-tick drift frame, and planning sizes B for catch-up frames.
- At 144 Hz and N=50k, one tick per frame still gave 17 and 5 hitches per 5 s (red team, measured), which refutes the earlier computed claim. That 10x load settles at 72 Hz in HalfRate without forks (computed), or holds 144 Hz with forks (1.18 ms ticks, measured).
- Permanent se_bench scenes: N=50k plus a spike at 60 Hz, N=30k for 60 s at 59.94 Hz, and N=50k at 144 Hz.

## Lifecycle

### States

- Booting
- Running
- Running.Overloaded (reported slowdown)
- Running.HalfRate (every second vblank, above 100 Hz)
- Modal (Windows drag, resize or menu loop, seen by the window subclass)
- FocusLost (visible, unfocused)
- Idle (editor in Edit mode, no input for 1 s)
- Suspended (minimized, hidden, occluded or Wayland-suspended)
- DeviceLost (device-removed status only)
- Hung (no progress for 5 consecutive 1 s periods)
- Faulted (game build, fatal)
- QuitPending (prompt and Durable saves)
- ShuttingDown
- Exited
- Play.Edit (document World, never stepped)
- Play.Playing
- Play.Paused
- Play.Stepping (exactly one tick)
- Play.Faulted (play World frozen read-only)

### Transitions

#### Booting → Running

**From:** Booting

**To:** Running

**Trigger:** SDL_AppInit succeeds

**Actions:** On main, in order:
- Start the Crashpad handler and register its streams.
- Configure log rings and sinks before other threads exist.
- SDL_Init(VIDEO|GAMEPAD) with SDL_HINT_AUTO_UPDATE_JOYSTICKS=0; install the event filter.
- Resolve the content root and pref path; create and subclass the window.
- Create the SDL_GPU device (patched SDL) and claim the window with VSYNC and the waitable.
- Pick Paced when present statistics and the vblank sampler agree with the display mode, else Standard.
- Read the CPU topology.
- Start the log worker, lane, input sampler, vblank sampler (Paced) and watchdog (boot budget only). Create the pool with no threads while W = 0.
- Init miniaudio (null device on failure) and ImGui (editor).
- Register the GameModule; pre-create storages.
- Set *appstate. The watchdog arms after the first iterate.

#### Booting → Exited

**From:** Booting

**To:** Exited

**Trigger:** An init step returns an Error or throws

**Actions:** Show a message box with the error's path and line. RAII inside App::create undoes the partial init, and *appstate stays NULL. Return SDL_APP_FAILURE. SDL_AppQuit sees NULL and only shuts logging down.

#### Running → Running.Overloaded (reported slowdown)

**From:** Running

**To:** Running.Overloaded (reported slowdown)

**Trigger:** 2 consecutive missed vblanks (present statistics) with tick debt, or k_95 < k_rt for 1 s; never while Modal

**Actions:** k_max = clamp(k_95, 1, k_rt), or 0..k_rt in editor Play. Dilate the excess. Show a SLOWDOWN badge, log once with the 3 costliest systems, send a Tracy message and mark the flight recorder.

#### Running.Overloaded (reported slowdown) → Running

**From:** Running.Overloaded (reported slowdown)

**To:** Running

**Trigger:** 1 s with no miss and k_95 >= k_rt

**Actions:** Restore normal k_max; log the total dilation.

#### Running → Running.HalfRate (every second vblank, above 100 Hz)

**From:** Running

**To:** Running.HalfRate (every second vblank, above 100 Hz)

**Trigger:** Display above 100 Hz and more than 1% of frames missed over 2 s

**Actions:** Present every second vblank: Paced uses the vblank sampler's phase; Standard re-presents the last composite. Recompute cadence for 2T; log.

#### Running.HalfRate (every second vblank, above 100 Hz) → Running

**From:** Running.HalfRate (every second vblank, above 100 Hz)

**To:** Running

**Trigger:** 5 s in which p95(C+g) + m stays below 0.75*T

**Actions:** Return to every vblank; log the dwell.

#### Running → Modal (Windows drag, resize or menu loop, seen by the window subclass)

**From:** Running

**To:** Modal (Windows drag, resize or menu loop, seen by the window subclass)

**Trigger:** WM_ENTERSIZEMOVE or WM_ENTERMENULOOP

**Actions:** Set the Modal flag. SDL resets the keyboard, so held input clears. SDL's WM_TIMER drives iterates at about 64-100 Hz (estimated). No waitable wait, no paced sleep, no B adaptation; misses are not counted and Overloaded cannot be entered. A NULL acquire skips the present. The cap still applies.

#### Modal (Windows drag, resize or menu loop, seen by the window subclass) → Running

**From:** Modal (Windows drag, resize or menu loop, seen by the window subclass)

**To:** Running

**Trigger:** WM_EXITSIZEMOVE or WM_EXITMENULOOP

**Actions:** Clear Modal; recalibrate the pacer; the swapchain resizes at the next acquire.

#### Running → FocusLost (visible, unfocused)

**From:** Running

**To:** FocusLost (visible, unfocused)

**Trigger:** SDL_EVENT_WINDOW_FOCUS_LOST

**Actions:** Clear held input. The game pushes its Pause state (configurable) and presents at 30 Hz at most. The editor keeps full rate only while Playing.

#### FocusLost (visible, unfocused) → Running

**From:** FocusLost (visible, unfocused)

**To:** Running

**Trigger:** SDL_EVENT_WINDOW_FOCUS_GAINED

**Actions:** Input resumes from a clean held state; normal pacing.

#### Running → Idle (editor in Edit mode, no input for 1 s)

**From:** Running

**To:** Idle (editor in Edit mode, no input for 1 s)

**Trigger:** No input for 1 s in Edit mode, no pending completions, no animated panel

**Actions:** SDL_HINT_MAIN_CALLBACK_RATE=waitevent; park the input sampler. The lane and dialog callbacks wake main through wakeMain().

#### Idle (editor in Edit mode, no input for 1 s) → Running

**From:** Idle (editor in Edit mode, no input for 1 s)

**To:** Running

**Trigger:** Any SDL event, including a wake event

**Actions:** Hint back to 0; redraw at once; resume the sampler.

#### Running → Suspended (minimized, hidden, occluded or Wayland-suspended)

**From:** Running

**To:** Suspended (minimized, hidden, occluded or Wayland-suspended)

**Trigger:** MINIMIZED, HIDDEN or OCCLUDED (the Wayland suspended state is assumed to arrive as OCCLUDED), or WM_POWERBROADCAST suspend. Never a NULL acquire.

**Actions:** Pause gameplay and count the time as paused. Skip rendering. SDL_HINT_MAIN_CALLBACK_RATE=10, so completions, uploads and audio sweeps continue. Park the samplers. Set the Suspended flag for the watchdog.

#### Suspended (minimized, hidden, occluded or Wayland-suspended) → Running

**From:** Suspended (minimized, hidden, occluded or Wayland-suspended)

**To:** Running

**Trigger:** RESTORED, SHOWN or EXPOSED; or, checked at 10 Hz, window flags show neither MINIMIZED nor HIDDEN and a probe acquire succeeds; or power resume

**Actions:** Hint back to 0. Reset the frame clock (no catch-up). Resume the samplers; recalibrate the pacer.

#### Running → DeviceLost (device-removed status only)

**From:** Running

**To:** DeviceLost (device-removed status only)

**Trigger:** The patched SDL reports device removal (D3D12 GetDeviceRemovedReason, Vulkan VK_ERROR_DEVICE_LOST). A NULL acquire or one failed submit never triggers it.

**Actions:** Stop rendering and pause gameplay. Release GPU objects through the retire queue and keep handles. Recreate the device, swapchain and pipelines. Re-read cooked assets on the lane (Urgent). Panels re-resolve their handles. Worlds are untouched.

#### DeviceLost (device-removed status only) → Running

**From:** DeviceLost (device-removed status only)

**To:** Running

**Trigger:** Recreation succeeds

**Actions:** Resume rendering; recalibrate the pacer.

#### Running → Hung (no progress for 5 consecutive 1 s periods)

**From:** Running

**To:** Hung (no progress for 5 consecutive 1 s periods)

**Trigger:** The iterate counter is unchanged for 5 periods on the unbiased clock, while not Suspended or Modal, with no heartbeat and no debugger

**Actions:** Request one out-of-process dump with the registered streams and the Play-start bytes. Label it a GPU or compositor stall if main's breadcrumb is inside an SDL_GPU wait. Never kill the process; never DebugBreak.

#### Hung (no progress for 5 consecutive 1 s periods) → Running

**From:** Hung (no progress for 5 consecutive 1 s periods)

**To:** Running

**Trigger:** Main makes progress

**Actions:** Log the episode, its duration and the dump id.

#### Running → Faulted (game build, fatal)

**From:** Running

**To:** Faulted (game build, fatal)

**Trigger:** An exception reaches any SDL callback's catch-all in the game build

**Actions:** Request a dump, show a message box, then run the shutdown order (no documents exist in the game build).

#### Faulted (game build, fatal) → ShuttingDown

**From:** Faulted (game build, fatal)

**To:** ShuttingDown

**Trigger:** Fatal fault handled

**Actions:** Run the shutdown order.

#### Running → QuitPending (prompt and Durable saves)

**From:** Running

**To:** QuitPending (prompt and Durable saves)

**Trigger:** SDL_EVENT_QUIT, window close or App::requestQuit

**Actions:** Set quitPending and keep returning SDL_APP_CONTINUE. Accept only the quit dialog's commands. If a file dialog is open, wait for its callback. Prompt about unsaved documents. Queue Save All, settings and the dock layout as Durable jobs, and show a progress modal.

#### QuitPending (prompt and Durable saves) → Running

**From:** QuitPending (prompt and Durable saves)

**To:** Running

**Trigger:** The user cancels, or a Durable save fails

**Actions:** A failed document stays dirty and shows its error; clear quitPending.

#### QuitPending (prompt and Durable saves) → ShuttingDown

**From:** QuitPending (prompt and Durable saves)

**To:** ShuttingDown

**Trigger:** Prompt resolved and every Durable job finished

**Actions:** The next SDL_AppIterate returns SDL_APP_SUCCESS; SDL calls SDL_AppQuit.

#### Running → ShuttingDown

**From:** Running

**To:** ShuttingDown

**Trigger:** WM_QUERYENDSESSION (seen by the subclass) or SDL_EVENT_TERMINATING

**Actions:** Write dirty documents synchronously on main to the recovery path (temp file, rename) and flush the logs. Then run the shutdown order as far as the OS allows.

#### ShuttingDown → Exited

**From:** ShuttingDown

**To:** Exited

**Trigger:** Shutdown order complete

**Actions:** Release builds terminate the process after the final flush. Debug, ASan and CI builds return from SDL_AppQuit, and SDL calls SDL_Quit.

#### Play.Edit (document World, never stepped) → Play.Playing

**From:** Play.Edit (document World, never stepped)

**To:** Play.Playing

**Trigger:** Play

**Actions:** Serialize the document into a fresh play World. Send the same bytes to the lane as a recovery file and keep them for bundles. Clone the AssetScope and freeze gameplay assets. Start a SessionLog, reset the accumulator and record the start tick.

#### Play.Playing → Play.Paused

**From:** Play.Playing

**To:** Play.Paused

**Trigger:** Pause

**Actions:** Stop stepping; prev := current once; reset the accumulator; render at alpha = 1; log Pause.

#### Play.Paused → Play.Stepping (exactly one tick)

**From:** Play.Paused

**To:** Play.Stepping (exactly one tick)

**Trigger:** Step

**Actions:** The next frame runs exactly one tick with a fresh InputFrame, logged, then returns to Paused.

#### Play.Paused → Play.Playing

**From:** Play.Paused

**To:** Play.Playing

**Trigger:** Play

**Actions:** Resume with the accumulator at 0 (no catch-up).

#### Play.Playing → Play.Faulted (play World frozen read-only)

**From:** Play.Playing

**To:** Play.Faulted (play World frozen read-only)

**Trigger:** An exception in a play tick (rethrown on main if it came from a worker) or in the play World's RenderList build

**Actions:** Abandon the tick and freeze the World. Render at alpha = 1 on a guarded path. Pick by brute force over storages. Disable edits. Save the replay, SessionLog, breadcrumbs and exception, and show the system, tick and message. A second render exception discards the World.

#### Play.Faulted (play World frozen read-only) → Play.Edit (document World, never stepped)

**From:** Play.Faulted (play World frozen read-only)

**To:** Play.Edit (document World, never stepped)

**Trigger:** Stop

**Actions:** Destroy the play World at the next frame start, releasing its scope through the retire queue. Assert the document hash is unchanged.

#### Play.Playing → Play.Edit (document World, never stepped)

**From:** Play.Playing

**To:** Play.Edit (document World, never stepped)

**Trigger:** Stop (also from Play.Paused)

**Actions:** At the next frame start, stop the World's voices, then destroy the World. Drop its pending completions. Unfreeze assets and apply deferred reloads. Assert the document hash is unchanged.

### Shutdown Order

- 1. QuitPending: keep iterating, resolve the prompt and drain every Durable job (saves, settings, dock layout). A failed save keeps its document dirty and cancels the quit. Quit waits while a file dialog is open.
- 2. The next SDL_AppIterate returns SDL_APP_SUCCESS and SDL calls SDL_AppQuit. No fork is in flight and no Durable job is pending.
- 3. Watchdog to shutdown mode. Its 5 s deadline starts now; on expiry it requests a dump, drains logs and terminates the process.
- 4. Remove the event filter; SDL_WaitForGPUIdle; run the retire queue.
- 5. Editor: destroy play Worlds and EditDocuments on main.
- 6. GameStateStack: destroy every World on main.
- 7. Audio: stop and ma_sound_uninit every voice, ma_engine_uninit (joins miniaudio's threads), then release PCM.
- 8. Background lane: request_stop; queued non-durable jobs are cancelled; a running step finishes (about 2 ms); join.
- 9. Input sampler: request_stop and join (within 1-4 ms). Vblank sampler: request_stop and join within 100 ms, or detach it if it is still in WaitForVBlank (assumption: it can block while the output is off).
- 10. AssetRegistry: release every scope.
- 11. ImGui: ImGui_ImplSDLGPU3_Shutdown, ImGui_ImplSDL3_Shutdown, DestroyContext.
- 12. Renderer: release pipelines, buffers and samplers; SDL_ReleaseWindowFromGPUDevice; SDL_DestroyGPUDevice.
- 13. ForkJoinPool: request_stop, bump the wake words, join.
- 14. Remove the window subclass; SDL_DestroyWindow.
- 15. SDL_QuitSubSystem(GAMEPAD | VIDEO), which joins SDL's HID, joystick and raw-input threads under the deadline.
- 16. Logging: point SDL_SetLogOutputFunction at stderr, drain and stop the log worker, then spdlog::shutdown(). On Windows this must happen before main returns.
- 17. Watchdog: request_stop, join. It is the last engine thread.
- 18. Release builds terminate the process (TerminateProcess or _exit), skipping CRT and DLL teardown, which have no deadline. Debug, ASan and CI builds return, and SDL calls SDL_Quit.

### Failure Handling

Workers: exceptions are captured per fork and the first one wins. Later chunks skip their work, and main rethrows after the join, abandoning the tick.

Main thread: every SDL callback is noexcept with a catch-all. In the editor, a play-tick or play-render exception moves the session to Play.Faulted, and the document is never touched. Anything else, and every exception in the game build, requests an out-of-process dump, shows a message box and runs the quit path. No exception unwinds through SDL's C frames or the Windows modal loop.

Rendering: everything that can throw runs before any GPU acquire. Recording is noexcept, and a guard submits whatever was recorded, never cancelling after an acquire. A NULL swapchain texture skips the present. Only a device-removed status enters DeviceLost.

Init: App::create returns std::expected and RAII undoes partial work. *appstate stays NULL and SDL_AppQuit accepts NULL. An audio init failure falls back to miniaudio's null backend.

Background jobs: failures come back as std::expected with Error{code, message, path, line, column}, and exceptions are converted the same way. A failed asset gets a placeholder. A failed Durable save keeps its document dirty and cancels a pending quit.

Audio: the callback is noexcept, never allocates and never waits. A device error sets an atomic flag, and main restarts the device or falls back to the null backend.

Fatal faults: an access violation or std::terminate reaches Crashpad's handler, which writes the dump out of process with the registered streams.

Hangs: the watchdog requests one dump per episode, in every build, after 5 consecutive 1 s periods without progress on the unbiased clock. It never DebugBreaks.

Shutdown: the 5 s deadline starts after the Durable drain and ends in termination. CI fails if teardown takes more than 2 s.

## Editor

Threading. ImGui and the whole editor run on main. Editor work that crosses a thread is limited to asset loads, saves, recovery and autosave writes (all on the lane), and native file dialogs (SDL's dialog thread, through a mailbox).

Documents. Each open scene or prefab is an EditDocument: a World that is never stepped, with its own undo stack, selection and dirty flag.

Reading state. Panels get a const World& after the frame's ticks, in both Edit and Play, so there is one inspector path and no mirror.
- Paused, stepping, edited and faulted Worlds render at alpha = 1, so the viewport matches the inspector.
- Picking uses the broadphase scratch when its validTick is current, then exact AABBs ordered by layer and z. A faulted World picks by brute force over registry storages (about 0.1 ms at 5-10k entities, estimated).
- Gizmos are ImGui overlays. A drag previews and commits one coalesced command.

Textures in panels. Panels hold Handle<Texture>, never ImTextureID or SDL_GPUTexture*, and call GpuResources::imguiId(handle) every frame. A CI grep rejects either type as a member in se_editor, and a debug RenderDrawData wrapper asserts that no bound texture was released this frame. Today's editor caches a texture-backed view across frames (app/editor/Editor.cpp:109); under SDL_GPU that pattern would read a freed texture container after a hot reload (red team C2).

Edits. Every edit is an EditorCommand: set, add or remove a component (before/after JSON), create or destroy an entity, or a tile or path command.
- Document edits apply after ImGui::Render and before the RenderList build, so they show in the same frame.
- Commands that release GPU resources (asset delete or reimport, document close, viewport resize) apply at the next frame start, because this frame's ImDrawData may still name their textures.
- A headless test fails on any Edit-mode mutation made outside commands.
- Edits during Play are queued in the SessionLog against the next tick and applied at that tick boundary (at once while paused). They are discarded at Stop.

Play controls take effect between frames and are logged with their tick index.
- Play serializes the document into a fresh play World, so every Play is a serializer round trip. The bytes go to the lane as a recovery file and are kept for bundles. Gameplay assets (AnimationSet, Prefab, stat schema, tileset collision) freeze until Stop; visual assets may hot reload and are logged.
- Pause stops stepping, sets prev := current once and resets the accumulator. Step runs exactly one tick.
- Stop destroys the play World at the next frame start, stopping its voices first, and asserts the document hash is unchanged. The document stores AssetIds, not asset content, so a hot reload cannot change that hash.
- 'Play game' runs the GameModule's GameStateStack.

Faults, hangs and quitting.
- An exception in a play tick or in the play World's RenderList build enters Play.Faulted: the World is frozen read-only, rendered at alpha = 1 on a guarded path, with edits disabled. The replay, SessionLog and breadcrumbs are saved.
- A hang freezes the editor too. The watchdog requests an out-of-process dump after 5 seconds without progress.
- Dirty documents autosave after 2 s with no input (Background class), and the next start offers recovery files.
- Quit is two-phase: the prompt and a progress modal run over normal frames, saves are Durable lane jobs, and the app exits only when all have finished. A failed save keeps its document dirty and cancels the quit. Quit waits while a file dialog is open. WM_QUERYENDSESSION writes recovery files synchronously.

File dialogs. SDL_ShowOpenFileDialog's callback runs on SDL's dialog thread. It copies the paths into a refcounted DialogMailbox, calls wakeMain() and returns; main acts at the next iterate.

Panels.
- Profiler: system ring buffers; ForkStats, including join p99.9, missed wakes and fallbacks; pacing (mode, cadence, B, frame latency, misses, HalfRate); overload; render stats; a flight-recorder dump button.
- Schedule: serial and parallel time per phase, and the state of each fork site.
- Log: lines formatted once into a main-owned ring, with dropped-record counts. Today the file is re-read every frame (core/util/LogStream.cpp:19-42).

Timing, pacing and power.
- io.DeltaTime is the real frame delta in seconds. Today milliseconds are passed as seconds (app/editor/Editor.cpp:136 with core/Engine.cpp:143).
- The editor defaults to Standard pacing with frame latency 2, so CPU and GPU overlap. 'Play game' uses Paced when the budget allows.
- In editor Play the Overloaded floor is 0 ticks: the simulation dilates (counted, with a badge) while the UI keeps at least 60 Hz. At 144-160 Hz the editor uses HalfRate instead of jittering.
- Idle editor: waitevent, woken by input or wakeMain(). Minimized: 10 Hz.
- A 60 Hz preview toggle. The stress generator spreads spawns across ticks and logs them.
- Editor work defaults to the Dev preset; the red team measured EnTT views 63-69x slower in /Od than in Release.

Viewports are offscreen SDL_GPU textures shown with ImGui::Image, recorded before the ImGui pass. PrepareDrawData runs before the render pass (dep/imgui-1.92.5/backends/imgui_impl_sdlgpu3.cpp:155-158). Multi-viewports are opt-in and run on main (697-734).

ImGui thread check: GImGui is thread_local in imconfig.h (the override documented at dep/imgui-1.92.5/imgui.cpp:1408-1414), so any ImGui call off main crashes at once.

Escalation cost: a dedicated simulation thread would need a Play-mode mirror for these panels.

## Input And Latency

Input path
- SDL pumps right after the pacer's wait. SDL_AppEvent passes each event to ImGui first (editor only), then to InputService, which keeps ordered, timestamped edges, held state and axes.
- Focus loss and the Windows modal loop clear held input. No system polls the keyboard; today it is polled once per rendered frame (app/scene/Sandbox.cpp:111-114).

Timestamp sources
- Keyboard: runtime builds turn on SDL_HINT_WINDOWS_RAW_KEYBOARD, so SDL's raw-input thread stamps keys with SDL_GetTicksNS. The editor leaves it off (IME and hotkeys) and uses Win32 message stamps, whose millisecond resolution is unverified.
- Gamepads: the input sampler calls SDL_UpdateGamepads at 1 kHz (250 Hz on battery) with auto-update off, so pad edges carry stamps of about 1 ms. With SDL's defaults, XInput would be polled once per frame: taps between polls would vanish and chord order would be lost (red team P10).
- Each edge records its source in the replay.

Placing edges on ticks
- Locked cadence: the k ticks of a frame split the wall time since the previous frame start into k equal windows.
- Free cadence: tick i's window ends at frameStart - remainder - (steps-1-i)*dt.
- Edges stamped after the last window go to the last tick. In a frame with no tick (one in six at 144 Hz), edges wait for the next frame's first tick, up to 6.9 ms; no edge waits longer.
- Frame data is in ticks, and a 6-12 tick buffer window absorbs sampling jitter.

Pacing modes
- Paced, the Windows runtime default on fixed-refresh displays:
  - The patched SDL swapchain has a frame-latency waitable (SetMaximumFrameLatency 1 or 2). The pacer waits on it, then sleeps until the vblank of the window's own output (from the vblank sampler) minus B(steps), the cost of the ticks already planned for that frame.
  - B = steps*S_hi + R_hi + g_hi + m, with p99.5 costs over 4 s and m = max(1.5 ms, timer p99 + 0.5 ms). Measured timer p99 was 519-659 us in self-tests and up to 940 us in runs.
  - Misses come from present statistics, not frame starts. Without them, DXGI flip model can settle into showing every frame one refresh late while starts stay on time (red team P1, inferred from DXGI and SDL source).
- VRR (user setting, when IMMEDIATE is supported): IMMEDIATE with ALLOW_TEARING at a self-paced 120 Hz deadline (60 Hz if the panel range is lower), one tick per frame, no miss or halving logic.
- Standard (the editor and Linux default): the waitable, then SDL_WaitForGPUSwapchain; no late latch.
- HalfRate above 100 Hz, with hysteresis. Frame latency 2 when CPU and GPU each fit but their sum does not.
- Linux: Standard on X11 or XWayland by default; Paced from VK_KHR_present_wait where the patched SDL and the driver support it. Parity is not claimed until measured.

Measured (rig, simulated vblank), input to present p50/p99
| Case | This design | Two threads, same pacing |
| Unpaced, 59.9 Hz, N=10k | 24.0 / 32.2 ms | 29.6 / 40.5 ms |
| Late-latched 12 ms (about Paced) | 11.9 / 20.2 ms | 17.6 / 28.6 ms |
| 144 Hz | 10.7 / 20.0 ms | 14.9 / 21.5 ms |
| Until fully past interpolation, p50 | 24.6 ms | 37.9 ms |
Real present and display latency is unmeasured. The harness uses PresentMon's displayed time plus a photodiode spot check, because fence plus DWM vblank cannot see a lost refresh.

Computed
- Paced, Locked cadence, 60 Hz, to mid-screen: about 22.1 ms mean (8.3 sampling + 5.5 B + 8.3 scanout), with the full effect shown.
- Paced, Free cadence, 144 Hz: about 12 ms to the first change at mid-screen, about 16 ms until fully past interpolation.
- Standard adds roughly one refresh; frame latency 2 adds another.

What Rampage tolerates: fighting games measure about 4-5 frames end to end at 60 Hz, display included (https://displaylag.com/video-game-input-lag-database/). The engine's target is at most 1.5 frames mean and 2 frames max at 60 Hz. Paced with Locked cadence gives about 1.3 and 1.8 frames (computed).

Why a sim thread does not help input: SDL delivers events only to the window thread, which also blocks on the swapchain. Gamepads are sampled by the input sampler in either design. The sim thread's measured cost was 4.25-5.7 ms p50, and phase lock could recover it only at 60 and 120 Hz, not at 144 Hz or the configured 160 Hz (config/config.json:19).

## Rendering

Renderer2D lives on main.

RenderList builder (headless). It reads a RenderSource (today a const World plus alpha) and:
- interpolates (Free cadence only) and culls;
- collects sprites, animators, tilemap chunks, parallax, text, UI, debug primitives and the frame's visual-event list;
- sorts by layer, z or y, and material, and emits quad batches.
If its fork sites are ever enabled, culling, the key sort and vertex generation run in parallel into disjoint ranges of a transfer buffer mapped (cycle=true) before the fork. In the rig the comparable pass took 0.53 ms p50 for 10,202 visible sprites, so it stays serial at the targets.

The frame transaction (main only)
1. Build phase, which may throw: RenderList, vertices, ImGui::Render. Nothing GPU-side has been acquired yet.
2. FrameRecorder acquires the upload command buffer when uploads are pending (byte budget 8 MB per frame, assumed) and the frame command buffer. Recording is noexcept: buffers are pre-sized and only engine-provided draw callbacks run.
3. Copy pass for vertices, then offscreen viewport passes, then the non-blocking swapchain acquire. NULL skips the swapchain pass and is never a state change; the command buffer is still submitted.
4. Swapchain pass, with ImGui's PrepareDrawData before it.
5. The guard ends any open pass, submits with SDL_SubmitGPUCommandBufferAndAcquireFence, never cancels after an acquire (SDL forbids it), stores the fence for the N-L wait, and runs the retire queue.
Every SDL_GPU wrapper asserts SDL_IsMainThread(), because SDL does not check.

Resource lifetime
- Retire queue: every SDL_ReleaseGPU* runs after the frame's submit. SDL's D3D12 backend frees the client texture container at release and defers only the GPU resource (red team C2).
- Textures are resolved from Handle{index, generation} every frame, and ImTextureIDs come from the same resolver.
- Uploads: a texture becomes resident (generation flip, swap, old texture retired) only when its upload fence signals, so skipped frames cannot leave garbage sprites.

Present
- VSYNC (Paced, Standard), IMMEDIATE with ALLOW_TEARING (VRR), MAILBOX opt-in.
- The frame-latency waitable bounds the queue at 1 or 2 frames (adaptive). SDL_SetGPUAllowedFramesInFlight stays at its default of 2. Present statistics give the vblank that showed each frame.
- HalfRate: Paced skips every second vblank by phase; Standard re-presents the last composite.

ImGui backend hazards stay on main: texture updates submit their own command buffer (dep/imgui-1.92.5/backends/imgui_impl_sdlgpu3.cpp:400-404) and buffer growth calls SDL_WaitForGPUIdle (line 136), so its buffers are pre-sized and growth is logged.

Rendering never writes simulation state: the builder takes a const source, so today's checkBoundary write (app/scene/Sandbox.cpp:155, 202-217) would not compile.

Device loss: only on a device-removed status from the patched SDL. Release through the retire queue, recreate on main, re-read cooked assets, re-resolve handles. Worlds are untouched.

GPU timing: per-command-buffer timestamps from the SDL patch feed g and Tracy GPU zones through native handles. Without them, g is unknown and k_max is held at k_rt whenever the previous fence had not signalled.

Stats per frame: draws, batches, quads, upload bytes, waitable and fence waits, missed vblanks, NULL acquires.

## Physics

Collision is first-party code. It runs on main as Collision-phase systems; its fork sites stay disabled until a measured miss.

Steps in each tick:
1. Rebuild a spatial hash from cached world AABBs: AABBs and a cell histogram per chunk, a serial prefix sum (about 1k cells), then a scatter into cell order.
2. Body pushes (role Body) are owner-computes over entity ranges in cell order: each entity scans its 3x3 neighbourhood and sums only its own push, writing only its own row. This kept the rig bit-identical with 0-6 workers.
3. Gameplay contacts (Hitbox, Hurtbox, Trigger) are generated per cell range. A pair is kept only in the cell holding the overlap's minimum corner, so no dedup set is needed. Pairs are filtered by layer/mask and role. Each chunk appends to its own growable output, and main concatenates in chunk order. Pile-ups make outputs grow per chunk instead of being pre-sized: 150 enemies on one spawn point give about 11k contacts in one range (computed). Pile-up and edge-of-world scenes are in the determinism and TSan suites.
4. Radix-sort contacts by (guidA, guidB) before any effect applies.
5. Resolve kinematic bodies against solids and tiles with swept AABBs.
Effects, cooldowns and hit rules then run serially in GameRules, in sorted-contact order.

Deterministic math. Simulation code uses first-party sin, cos and atan2 built from +, -, *, / and sqrt. It never calls the CRT's transcendental functions, whose x64 MSVC implementation picks FMA3 or non-FMA3 code by CPU (C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/ucrt/corecrt_math.h:631). _set_FMA3_enable(0) is called at startup as well. Builds use /fp:precise on MSVC and -ffp-contract=off elsewhere.

Measured in the rig (synthetic: uniform random AABBs, 0.29 contacts per entity per tick):
| Colliders | Tick p50, one core | Tick p50, 6 workers |
|---|---|---|
| 5k | 0.40 ms (p99 0.70) | 0.134 ms |
| 10k | 0.79 ms | 0.251 ms |
| 20k | 1.63 ms | 0.469 ms |
| 50k | 4.20 ms | 1.18 ms |
The narrowphase is where parallelism pays: 3426 to 542 us at 50k. The counting sort stays serial. In the red team's Debug build the rig tick rose from 0.42 to 1.27 ms, and a lookup-heavy EnTT sweep was 111x slower.

Box2D v3 is adopted only if dynamic rigid bodies are needed. Then:
- b2World_Step runs on main inside the tick, while no fork is active.
- Its enqueue and finish callbacks use a TaskSet on P-core workers: concurrent tasks with per-task latches and a bounded table, independent of the fork slot. Box2D's solver expects tasks to run concurrently with the caller, which the single-slot fork cannot provide (red team C20).
- One version is pinned, because the callback types changed between 3.1 and 3.2. Events are consumed right after the step.

## Audio And Assets

Assets

Ownership. The AssetRegistry lives on main: typed stores, Handle{index, generation}, an AssetScope per GameState and EditDocument, and a persistent UI scope.

Loading:
1. Main submits an AssetRequest to the lane, as Urgent when something waits on it.
2. The lane returns a payload or an Error through the SPSC completion ring.
3. At iterate start main drains the ring, creates the SDL_GPU objects, and queues uploads within a byte budget (8 MB per frame, assumed).
4. When an upload's fence signals, the texture becomes resident: its generation flips, and any old texture is retired.
A World never steps before its assets are resident, so asset timing cannot change simulation results.

Bounded lane steps. Every job is a sequence of steps of about 2 ms with a stop_token check between them:
- file reads of at most 1 MB;
- runtime atlas pages cooked as independent LZ4 blocks of about 256 KB;
- scenes split into chunks by the importer.
stb_image runs only at editor import; one large image is the only unsplittable step (tens of ms, estimated). Step times are estimates, measured in se_bench.

Writes. Saves, settings, recovery files and autosaves are written to a unique temp file and renamed into place.
- Writes to one path form a strand (first in, first out).
- Each file carries a revision, and the lane never renames over a newer one.
- Engine reads open files with FILE_SHARE_DELETE. A rename hitting another process's sharing violation retries for up to 2 s, then fails and leaves the document dirty.
- User-requested saves are Durable: never cancelled, and quit waits for them.

QoS. Urgent jobs (blocking loads, user imports, hot reload) run at HighQoS and normal priority. Background jobs (autosave, recovery, flushes, thumbnails, scans) run at EcoQoS. Urgent steps always go first.

Hot reload. The lane scans every 500 ms and wakes main on a change. Changed assets swap at frame start once resident, and the old texture goes through the retire queue. In editor Play, gameplay assets are frozen until Stop; visual assets swap and are logged in the SessionLog. This removes today's unload hazard (core/manager/ResourceManager.hpp:106-120).

Audio

Ownership. Main owns ma_engine, with Music and Effects groups whose volumes come from config/config.json:26-29. It is the only caller, because miniaudio does not document thread safety for these calls. If device init fails (as on CI runners and VMs), the engine uses miniaudio's null backend.

Voices. Each voice record holds its ma_sound and a shared_ptr to its PCM.
- PCM is released only after the voice's end id has drained and ma_sound_uninit has returned.
- Releasing a scope (Stop, scene change) first stops and uninitialises its voices; ma_sound_uninit is the fence.
- A hot-reloaded sound gives its new buffer to new voices only.

Scheduling. After each tick, main drains AudioEmitter changes and audio events. Each sound starts at max(now, tick anchor + D), set with ma_sound_set_start_time_in_pcm_frames.
- The two ticks of a 60 Hz frame play 8.33 ms apart.
- D = B + T/2 = 13.8 ms at 60 Hz Paced in Locked cadence (computed): the time from a tick's window end to its frame at mid-screen.
- Late gameplay sounds play at once instead of being dropped. A per-group voice cap with oldest-voice stealing bounds bursts after a stall. Only sounds flagged cosmetic are dropped when more than 100 ms late.

Threads. miniaudio mixes on its device thread and streams on its resource-manager thread. The end-of-sound callback writes ids into an SPSC ring sized for the voice cap (256) and never waits. A per-frame sweeper also uninitialises voices whose ma_sound_at_end is true. Suspended keeps iterating at 10 Hz, so the ring keeps draining. This replaces today's empty audio and resource threads (core/Engine.cpp:218-243).

## Determinism And Debugging

What determines the simulation:
- the scene bytes (in the editor, the Play-start bytes);
- the seed and dt;
- the InputFrame sequence, including where each edge lands;
- the SessionLog: every play-World mutation the Schedule did not produce (editor commands, spawns, asset swaps, GameState commands, Pause/Step/Stop), each applied at a recorded tick boundary;
- the build, the platform and the compiler.

Wall time reaches only the FixedStepper and the SessionLog's choice of tick: how many ticks run, where edges land, and before which tick a logged mutation applies. All three are recorded; none decides what a tick computes. GameState transitions apply at tick boundaries, so a GameOver requested in a frame's first tick no longer lets the second tick run in the old World (red team C6). Runtime Guids derive from (parent Guid, tick, spawn index).

Systems read only SimClock. A banned-API check rejects steady_clock, system_clock, SDL_GetTicks and <cmath> transcendental functions in se_core and game systems. Today, effects use steady_clock (core/system/EventSystem.cpp:95,135). One thread makes every structural change in program order, so EnTT's pool order is deterministic.

Fork-join keeps results independent of the thread count: fixed chunking, per-chunk outputs merged in chunk order, fixed-order reductions, no floating-point atomics, contacts sorted before effects, counter-based RNG. Measured: identical hashes with 0, 2, 4 and 6 workers (628deeea45180d2e at N=10k, 833864191b0999b5 at N=50k).

Floating point: /fp:precise on MSVC, -ffp-contract=off elsewhere, first-party sin/cos/atan2, and _set_FMA3_enable(0). The MSVC x64 CRT picks FMA3 or non-FMA3 code by CPU, so a field replay could diverge (red team P13). Replay headers carry CPU feature bits and a startup math self-test hash. Golden hashes are kept per platform and compiler.

Record and replay
- InputFrames go into a 60 s ring (about 0.46 MB, computed) with seqlock-stamped slots and a published head. The SessionLog and Play-start bytes go with it, and the lane flushes them.
- Headers carry the build hash, platform, CPU features, math hash, scene hash, seed and dt.
- A headless runner compares a Guid-keyed hash every tick. Every Play abort, crash and hang bundle includes the ring, the SessionLog and the Play-start bytes.

Running on one thread: SE_JOBS=0, SE_BG_INLINE=1, --fixed-frame-dt=<ms>, --det-crosscheck (clones the World every K ticks, steps it serially, reports the first difference), --break-on-overload, SE_CHAOS_SEED. A breakpoint stops the only mutator, and with a debugger attached a stall over 100 ms counts as paused time.

Build presets. Editor work defaults to the Dev preset: /O2 /Zi /Zo /Oy-, _ITERATOR_DEBUG_LEVEL=0 for the engine and every vendored dependency, engine and EnTT asserts on, and #pragma optimize("", off) per file when stepping. The red team measured EnTT views 63-69x and a lookup-heavy sweep 111x slower in /Od, which would make a 120 Hz Play session dilate at the targets. se_bench budgets run in Dev and Release.

Crash and hang capture
- The Crashpad handler (Windows and Linux) writes every dump out of process; the watchdog and the exception filter only request one. No engine DbgHelp call runs in process, and TRACY_DBGHELP_LOCK serialises Tracy's own use.
- Registered streams in every dump:
  - breadcrumbs {role:4, phase:4, system:12, chunk:16, tick:28};
  - log rings (the last 4096 records on main, 1024 on other threads);
  - the InputFrame ring;
  - the SessionLog tail;
  - the pacing flight recorder;
  - system info.
  The Play-start bytes are attached.
- Hangs: 5 consecutive 1 s periods without progress on QueryUnbiasedInterruptTime or CLOCK_MONOTONIC, armed after the first iterate. In-driver waits are labelled GPU or compositor stalls. Never DebugBreak.

Pacing flight recorder. Always on: 32 B per frame (start, steps, k_max, B, CPU and GPU time, fence state, miss, mode, cadence, frame latency, display, refresh, present mode), 2.8 MB for 600 s at 144 Hz (computed), plus per-system costs for each missed frame. Dumped on a hotkey, with --flight-dump, automatically above 0.5% misses per minute (at most once per 10 min), and in every bundle. System info: CPU model, EfficiencyClass layout, power source (SDL_GetPowerInfo), QoS applied, GPU and driver, display modes, VRR setting.

Tooling
- Tracy:
  - named threads (main, fj-worker-N, bg-lane, log-worker, watchdog, input-sampler, vblank-sampler);
  - FrameMark per display frame and FrameMarkNamed per tick;
  - zones per system and fork;
  - GPU zones through the patched SDL's native handles;
  - plots of alpha, ticks per frame, k_max, B, frame latency, debt and dilation.
- TSan on Linux Clang: the determinism suite at W of 1, 3 and 7, pool property tests with chaos and pile-up scenes, inbox and dialog tests, and shutdown stress.
- Clang -Werror=thread-safety with MainThreadRole, to confirm on the pinned Clang.
- Runtime asserts: SE_ASSERT_MAIN_THREAD in every SDL, GPU and ImGui wrapper; SE_ASSERT_NOT_IN_FORK in structural calls, the dispatcher and World-level sinks; ParallelView range and set checks; thread_local GImGui.
- ASan and UBSan.

## Platform

### SDL_Init(VIDEO), windows, window queries and the event pump run on SDL's main thread

**Constraint:** SDL_Init(VIDEO), windows, window queries and the event pump run on SDL's main thread

**How Honoured:** Main owns all of them; other threads get values.

**Source:** https://wiki.libsdl.org/SDL3/SDL_Init ; https://wiki.libsdl.org/SDL3/SDL_PollEvent

### Swapchain claim, acquire and wait, and the presenting command buffer, run on the window's thread; SDL_GPU performs no thread checks

**Constraint:** Swapchain claim, acquire and wait, and the presenting command buffer, run on the window's thread; SDL_GPU performs no thread checks

**How Honoured:** Every SDL_GPU call is on main, and every wrapper asserts SDL_IsMainThread().

**Source:** https://wiki.libsdl.org/SDL3/SDL_AcquireGPUSwapchainTexture ; https://github.com/libsdl-org/SDL/blob/main/src/gpu/SDL_gpu.c

### With main callbacks, SDL's watcher dispatches six lifecycle event types immediately on the pushing thread and drains the whole queue there. An event filter runs before watchers on the pushing thread, and returning false drops the event.

**Constraint:** With main callbacks, SDL's watcher dispatches six lifecycle event types immediately on the pushing thread and drains the whole queue there. An event filter runs before watchers on the pushing thread, and returning false drops the event.

**How Honoured:** The event filter copies off-main immediate events into the inbox and returns false. Main drains the inbox at iterate start with no SDL call under its lock. Engine threads push only wake user events.

**Source:** https://raw.githubusercontent.com/libsdl-org/SDL/main/src/main/SDL_main_callbacks.c ; https://raw.githubusercontent.com/libsdl-org/SDL/main/src/events/SDL_eventwatch.c ; https://wiki.libsdl.org/SDL3/SDL_SetEventFilter

### Once a callback returns SUCCESS or FAILURE, SDL stops iterating and calls SDL_AppQuit. SDL_AppQuit is called even after SDL_AppInit fails. Exceptions must not unwind through SDL's C frames.

**Constraint:** Once a callback returns SUCCESS or FAILURE, SDL stops iterating and calls SDL_AppQuit. SDL_AppQuit is called even after SDL_AppInit fails. Exceptions must not unwind through SDL's C frames.

**How Honoured:** Two-phase quit: callbacks return CONTINUE until the Durable drain ends. *appstate is set only after a complete init, and SDL_AppQuit accepts NULL. All callbacks are noexcept with catch-alls.

**Source:** https://raw.githubusercontent.com/libsdl-org/SDL/main/src/main/generic/SDL_sysmain_callbacks.c ; https://wiki.libsdl.org/SDL3/SDL_AppQuit

### The Windows modal drag loop blocks the pump. SDL keeps its modal state private, and sent messages bypass SDL_SetWindowsMessageHook.

**Constraint:** The Windows modal drag loop blocks the pump. SDL keeps its modal state private, and sent messages bypass SDL_SetWindowsMessageHook.

**How Honoured:** Main callbacks keep iterating from WM_TIMER through SDL_OnWindowLiveResizeUpdate. A SetWindowSubclass hook on main catches enter/exit size-move and menu-loop messages and sets Modal.

**Source:** https://raw.githubusercontent.com/libsdl-org/SDL/main/src/video/SDL_video.c ; https://raw.githubusercontent.com/libsdl-org/SDL/main/src/video/windows/SDL_windowsevents.c ; https://learn.microsoft.com/en-us/windows/win32/api/commctrl/nf-commctrl-setwindowsubclass

### SDL file-dialog callbacks may run on another thread. On Windows SDL always runs the dialog on a detached thread, owned by the app window.

**Constraint:** SDL file-dialog callbacks may run on another thread. On Windows SDL always runs the dialog on a detached thread, owned by the app window.

**How Honoured:** Callbacks only fill a refcounted mailbox and call wakeMain(). Quit waits while a dialog is open, and main never waits on that thread.

**Source:** https://wiki.libsdl.org/SDL3/SDL_ShowOpenFileDialog ; https://raw.githubusercontent.com/libsdl-org/SDL/main/src/dialog/windows/SDL_windowsdialog.c

### SDL_HINT_MAIN_CALLBACK_RATE=waitevent stops iterating until an event arrives

**Constraint:** SDL_HINT_MAIN_CALLBACK_RATE=waitevent stops iterating until an event arrives

**How Honoured:** Used only for the idle editor, which is woken by input and wakeMain(). Suspended uses a rate of 10.

**Source:** https://wiki.libsdl.org/SDL3/SDL_HINT_MAIN_CALLBACK_RATE

### ImGui has one global context, and imgui_impl_sdl3 NewFrame calls main-thread-only SDL functions

**Constraint:** ImGui has one global context, and imgui_impl_sdl3 NewFrame calls main-thread-only SDL functions

**How Honoured:** ImGui runs on main, and GImGui is thread_local, so a stray call crashes at once.

**Source:** dep/imgui-1.92.5/imgui.cpp:1405-1420; dep/imgui-1.92.5/backends/imgui_impl_sdl3.cpp:970-1014

### imgui_impl_sdlgpu3 uses a raw SDL_GPUTexture* as ImTextureID and binds it at RenderDrawData time. It needs PrepareDrawData before the pass, submits its own upload buffer, stalls on growth, and presents viewports on the calling thread.

**Constraint:** imgui_impl_sdlgpu3 uses a raw SDL_GPUTexture* as ImTextureID and binds it at RenderDrawData time. It needs PrepareDrawData before the pass, submits its own upload buffer, stalls on growth, and presents viewports on the calling thread.

**How Honoured:** All on main. IDs are resolved from handles every frame and never stored. Buffers are pre-sized; multi-viewports are opt-in.

**Source:** dep/imgui-1.92.5/backends/imgui_impl_sdlgpu3.cpp:5, 136, 155-158, 288-290, 400-404, 697-734

### On D3D12, SDL_ReleaseGPUTexture frees the client texture container at once; only the GPU resource is deferred

**Constraint:** On D3D12, SDL_ReleaseGPUTexture frees the client texture container at once; only the GPU resource is deferred

**How Honoured:** A retire queue runs releases after the frame's submit. Resource-releasing editor commands apply at the next frame start.

**Source:** https://raw.githubusercontent.com/libsdl-org/SDL/main/src/gpu/d3d12/SDL_gpu_d3d12.c (red team C2)

### SDL_CancelGPUCommandBuffer is forbidden after a swapchain acquire. A NULL swapchain texture is not an error: too many frames in flight, or a window that cannot present now.

**Constraint:** SDL_CancelGPUCommandBuffer is forbidden after a swapchain acquire. A NULL swapchain texture is not an error: too many frames in flight, or a window that cannot present now.

**How Honoured:** A guard always submits. Everything that can throw runs before the acquire. NULL only skips the present.

**Source:** https://wiki.libsdl.org/SDL3/SDL_CancelGPUCommandBuffer ; https://wiki.libsdl.org/SDL3/SDL_AcquireGPUSwapchainTexture ; https://raw.githubusercontent.com/libsdl-org/SDL/main/src/gpu/vulkan/SDL_gpu_vulkan.c

### Without a frame-latency waitable, DXGI flip model can settle into showing every frame one refresh late while frame starts stay on time. SDL's D3D12 backend creates no waitable and keeps at least 2 buffers.

**Constraint:** Without a frame-latency waitable, DXGI flip model can settle into showing every frame one refresh late while frame starts stay on time. SDL's D3D12 backend creates no waitable and keeps at least 2 buffers.

**How Honoured:** A small SDL patch adds the waitable with SetMaximumFrameLatency(1 or 2) and exposes present statistics. Misses come from those statistics.

**Source:** https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains ; https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getframestatistics

### DwmGetCompositionTimingInfo takes hwnd = NULL since Windows 8.1, so its timing is desktop-wide

**Constraint:** DwmGetCompositionTimingInfo takes hwnd = NULL since Windows 8.1, so its timing is desktop-wide

**How Honoured:** A vblank sampler blocks in IDXGIOutput::WaitForVBlank on the window's own output. A mismatch with the display mode drops to Standard.

**Source:** https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmgetcompositiontiminginfo ; https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgioutput-waitforvblank

### Windowed VRR needs sync interval 0 with ALLOW_TEARING, which SDL sets only for IMMEDIATE. SDL_GPU has no sync interval 2, no timestamp queries and no native handles.

**Constraint:** Windowed VRR needs sync interval 0 with ALLOW_TEARING, which SDL sets only for IMMEDIATE. SDL_GPU has no sync interval 2, no timestamp queries and no native handles.

**How Honoured:** VRR mode presents IMMEDIATE at a self-paced 120 Hz. HalfRate uses vblank phase or re-presents. The SDL patch adds timestamps and handles; without them g is unknown.

**Source:** https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays ; https://github.com/libsdl-org/SDL/blob/main/include/SDL3/SDL_gpu.h

### Vulkan FIFO present and acquire can block on Linux, indefinitely in some compositor states. xdg-shell cannot report minimize.

**Constraint:** Vulkan FIFO present and acquire can block on Linux, indefinitely in some compositor states. xdg-shell cannot report minimize.

**How Honoured:** Linux defaults to Standard on X11/XWayland. In-driver waits are flagged and counted as paused time. The suspended toplevel state enters Suspended (assumed to arrive as OCCLUDED). Blocking seen on target hardware is a flip condition.

**Source:** https://github.com/libsdl-org/SDL/issues/11293 ; https://github.com/libsdl-org/SDL/issues/16548 ; https://wayland.app/protocols/xdg-shell

### The EnTT registry is not thread-safe. Non-const access creates storages lazily. ENTT_USE_ATOMIC is required with threads.

**Constraint:** The EnTT registry is not thread-safe. Non-const access creates storages lazily. ENTT_USE_ATOMIC is required with threads.

**How Honoured:** Structural changes happen only on main, outside forks. Storages are pre-created. ENTT_USE_ATOMIC is defined; nothing defines it today.

**Source:** dep/entt-3.16.0/docs/md/entity.md:2333-2373, 2408-2433; dep/entt-3.16.0/src/entt/entity/registry.hpp:257-293

### EnTT signals fire inside the mutating call, so on_construct can run before later components exist. Versions wrap after 4095 reuses.

**Constraint:** EnTT signals fire inside the mutating call, so on_construct can run before later components exist. Versions wrap after 4095 reuses.

**How Honoured:** No on_construct interpolation hook: a spawn-finalize pass runs in Cleanup instead. Nothing is matched by id across states.

**Source:** dep/entt-3.16.0/src/entt/entity/mixin.hpp:79-112; dep/entt-3.16.0/src/entt/entity/entity.hpp:38-39

### Do not toggle timeBeginPeriod per wait. High-resolution waitable timers exist from Windows 10 1803. Relative timers that came due during sleep expire at wake.

**Constraint:** Do not toggle timeBeginPeriod per wait. High-resolution waitable timers exist from Windows 10 1803. Relative timers that came due during sleep expire at wake.

**How Honoured:** SDL_DelayNS with a margin above the measured timer p99, never spinning. The watchdog uses QueryUnbiasedInterruptTime over 5 periods and treats power suspend as Suspended.

**Source:** https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod ; https://learn.microsoft.com/en-us/windows/win32/sysinfo/interrupt-time ; dep/SFML-3.0.2/src/SFML/System/Win32/SleepImpl.cpp:47-54

### Windows QoS lowers unfocused or hidden windows, and EcoQoS always picks efficient cores. SRW locks have no priority inheritance, and spdlog's async consumer dequeues under its mutex.

**Constraint:** Windows QoS lowers unfocused or hidden windows, and EcoQoS always picks efficient cores. SRW locks have no priority inheritance, and spdlog's async consumer dequeues under its mutex.

**How Honoured:** Main, workers and samplers run HighQoS with CPU sets. Lane QoS is set per job; the log worker uses default QoS. Main shares no lock with EcoQoS threads (SPSC rings, try_lock).

**Source:** https://learn.microsoft.com/en-us/windows/win32/procthread/quality-of-service ; https://raw.githubusercontent.com/gabime/spdlog/v1.x/include/spdlog/details/mpmc_blocking_q.h

### The window thread must never wait on a thread that may wait on it (DXGI). Owned dialogs send messages to their owner window.

**Constraint:** The window thread must never wait on a thread that may wait on it (DXGI). Owned dialogs send messages to their owner window.

**How Honoured:** Main waits only on fork joins and the GPU. No engine thread waits on main, and main never waits on library threads outside shutdown.

**Source:** https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi

### DbgHelp is single-threaded, and MiniDumpWriteDump should be called from another process (loader deadlocks). MSVC's x64 CRT picks FMA3 paths by CPU.

**Constraint:** DbgHelp is single-threaded, and MiniDumpWriteDump should be called from another process (loader deadlocks). MSVC's x64 CRT picks FMA3 paths by CPU.

**How Honoured:** Crashpad writes dumps out of process, with TRACY_DBGHELP_LOCK for Tracy. _set_FMA3_enable(0) and first-party trig in simulation code.

**Source:** https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump ; https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/get-fma3-enable-set-fma3-enable ; C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/ucrt/corecrt_math.h:631

### SDL_UpdateGamepads is safe from any thread. Joysticks update during event pumping unless SDL_HINT_AUTO_UPDATE_JOYSTICKS=0.

**Constraint:** SDL_UpdateGamepads is safe from any thread. Joysticks update during event pumping unless SDL_HINT_AUTO_UPDATE_JOYSTICKS=0.

**How Honoured:** The input sampler polls at 1 kHz with auto-update off.

**Source:** https://wiki.libsdl.org/SDL3/SDL_UpdateGamepads ; https://wiki.libsdl.org/SDL3/SDL_HINT_AUTO_UPDATE_JOYSTICKS

### miniaudio: never init, uninit, start or stop the device from its callback. ma_sound_uninit waits for the audio thread. ma_engine thread safety is undocumented.

**Constraint:** miniaudio: never init, uninit, start or stop the device from its callback. ma_sound_uninit waits for the audio thread. ma_engine thread safety is undocumented.

**How Honoured:** Main is the single owner. The callback only writes an SPSC ring. ma_sound_uninit fences PCM release.

**Source:** https://miniaud.io/docs/manual/index.html

### spdlog: configure sinks before threads start, and call shutdown before main returns on Windows. atomic<shared_ptr> is not lock-free on MSVC or libstdc++.

**Constraint:** spdlog: configure sinks before threads start, and call shutdown before main returns on Windows. atomic<shared_ptr> is not lock-free on MSVC or libstdc++.

**How Honoured:** Sinks are configured at boot and used only by the log worker; shutdown is step 16. SPSC rings are used instead of atomic<shared_ptr>.

**Source:** https://github.com/gabime/spdlog/wiki/Asynchronous-logging ; MSVC 14.51 include/memory:4093-4100 (constraints research)

### Box2D: one stepping thread and no world access during a step. Its solver expects enqueued tasks to run concurrently with the caller.

**Constraint:** Box2D: one stepping thread and no world access during a step. Its solver expects enqueued tasks to run concurrently with the caller.

**How Honoured:** If adopted, it steps on main inside the tick, with a TaskSet on P-core workers.

**Source:** https://box2d.org/documentation/md_foundation.html ; https://raw.githubusercontent.com/erincatto/box2d/main/src/solver.c

## API sketch

```cpp
// ===== se_core/thread_role.hpp =====
namespace se {
struct SE_CAPABILITY("role") MainThreadRole {};
extern MainThreadRole g_main;                       // SE_ACQUIRE(g_main) once in SDL_AppInit
enum class ThreadRole : std::uint8_t { Library, Main, ForkWorker, Background, LogWorker, Watchdog, InputSampler, VblankSampler };
#define SE_ASSERT_MAIN_THREAD() SE_ASSERT(SDL_IsMainThread())
#define SE_ASSERT_NOT_IN_FORK() SE_ASSERT(!::se::jobs::t_inFork)
struct alignas(64) Breadcrumb { std::atomic<std::uint64_t> packed; };  // role:4 phase:4 system:12 chunk:16 tick:28
void wakeMain() noexcept;                           // SDL_PushEvent(engine wake user event): the only event engine code pushes
}

// ===== se_core/jobs.hpp =====
namespace se::jobs {
enum class CoreClass : std::uint8_t { AnyPhysical, PerformanceOnly };
struct ForkSite {
  std::string_view name; std::uint32_t grain;       // chunk >= 10 us of measured work
  std::uint32_t minItems; CoreClass cores = CoreClass::AnyPhysical;
  std::span<const entt::id_type> reads, writes;     // debug: ParallelView rejects undeclared access
  bool enabled = false;                             // miss + TSan chaos + overhead <= 10% + 10-min tail soak
  std::uint64_t inlineUntilNs = 0;                  // join > 4x median -> inline for 5 s
  ForkStats stats;                                  // join p50/p99.9/max, late and missed wakes, fallbacks
};
class ForkJoinPool {
public:
  struct Config { int workers = 0;                  // shipped default: no threads
                  int maxWorkers = -1;              // physical cores - 2 (homogeneous 8+: - 1), cap 15
                  std::uint32_t spinUs = 20; std::uint64_t chaosSeed = 0; };
  template<class F> void parallelFor(ForkSite&, std::uint32_t count, F&&) SE_REQUIRES(g_main); // participants = min(W+1, chunks/4)
private:
  struct alignas(64) Desc { void (*invoke)(void*, ChunkRange); void* ctx; std::uint32_t count, grain, chunks, batch; };
  Desc m_desc;                                             // copied to locals once per generation
  alignas(64) std::atomic<std::uint32_t> m_remaining{0};   // own line
  alignas(64) Failure m_fail;                              // atomic<bool> + exception_ptr
  alignas(64) std::atomic<std::uint64_t> m_claim{0};       // [gen:16 | chunks:24 | next:24], fetch_add(batch, acq_rel)
  std::array<PaddedAtomicU32, 15> m_wake;                  // worker: old = load(acquire); tryClaim(); wait(old)
};
class TaskSet { /* only if Box2D is adopted: concurrent tasks with latches, bounded table, independent of the fork slot */ };
}

// ===== se_core/background_lane.hpp =====
enum class JobClass : std::uint8_t { Urgent, Background };     // HighQoS / EcoQoS, switched per step
struct JobDesc { JobClass cls; bool durable = false;           // durable: never cancelled; quit waits
                 std::uint64_t strand = 0, revision = 0; };    // strand = destination path; never overwrite a newer revision
class BackgroundLane {                                         // 1 thread; SPSC rings; binary_semaphore (500 ms)
public:
  Ticket submit(JobDesc, Step) SE_REQUIRES(g_main);           // Step returns within ~2 ms, checks stop_token
  bool durableIdle() const noexcept;
  template<class F> void drainCompletions(F&&) SE_REQUIRES(g_main);
};

// ===== se_core/time.hpp =====
enum class Cadence : std::uint8_t { Locked, Free };            // Locked: period within 0.5% of k*dt or dt*k
struct FrameCosts { double tickHi, nonTickHi, gpuHi;           // p99.5 over 4 s (B, extra tick)
                    double tick95, nonTick95, gpu95, margin; };// p95 over 2 s (overload); gpu NaN if unknown
class FixedStepper {                                // pure, unit-tested
public:
  StepPlan plan(const FrameClock&, const FrameCosts&, bool running, bool debugger, bool editorPlay) noexcept;
  StepPlan planNext(const FrameClock&, const FrameCosts&) const noexcept;   // before the late-latch sleep
  // Normal: k_max = clamp(max(k_rt, k_hi), 1, min(8, k_rt + 1)); Overloaded: clamp(k_95, editorPlay ? 0 : 1, k_rt)
  void onPresentResult(bool missed, bool modal) noexcept;
};

// ===== se_platform/input.hpp =====
enum class EdgeSource : std::uint8_t { RawKeyboard, WinMessage, GamepadSampler, Mouse };
struct InputEdge { ActionId action; bool down; std::uint64_t stampNs; std::uint8_t order; EdgeSource source; };
InputFrame InputService::latch(std::uint64_t tick, std::uint64_t windowEndNs, bool lastTickOfFrame) SE_REQUIRES(g_main);

// ===== se_core/world.hpp =====
class World {
public:
  entt::entity create(Guid) SE_REQUIRES(g_main);    // asserts main + !inFork; marks for spawn finalize
  static Guid deriveGuid(Guid parent, std::uint64_t tick, std::uint32_t index) noexcept;
  void finalizeSpawns() SE_REQUIRES(g_main);        // Cleanup and after load: prev = current, teleportTick = spawnTick
  template<class... C> ParallelView<C...> parallel(const jobs::ForkSite&) const;   // range + set checked in debug
};

// ===== se_runtime/sim_driver.cpp: the seam =====
void SimDriver::tickInline(GameStateStack& states, const StepPlan& p, InputService& in, Schedule& s,
                           jobs::ForkJoinPool& jobs, SessionLog& log, ReplayRecorder& rec, AudioBridge& audio) {
  for (int i = 0; i < p.steps; ++i) {
    const std::uint64_t t = nextTick();
    states.applyPending();                           // GameState commands from the previous tick (C6)
    log.applyDue(t, states);                         // edits, spawns, asset swaps due before tick t (C7)
    const InputFrame f = in.latch(t, p.tickWindowEndNs[i], i + 1 == p.steps);
    rec.record(f);
    for (World* w : states.runningWorlds()) { s.step(*w, f, jobs); audio.drain(*w, p.tickWindowEndNs[i]); }
  }
}

// ===== se_render =====
class GpuResources {
public:
  SDL_GPUTexture* resolve(Handle<Texture>) SE_REQUIRES(g_main);     // valid this frame only
  ImTextureID imguiId(Handle<Texture>) SE_REQUIRES(g_main);         // panels never store the result
  void retire(SDL_GPUTexture*) SE_REQUIRES(g_main);                 // released after this frame's submit
  void queueUpload(Handle<Texture>, Payload&&) SE_REQUIRES(g_main); // resident when its fence signals
};
class FrameRecorder {                               // RAII; noexcept after construction
public:
  SDL_GPUTexture* acquireSwapchain(SDL_Window*) noexcept;   // NULL: skip present, never a state change
  ~FrameRecorder();                                          // end passes, submit (never cancel), keep fence, run retire queue
};

// ===== se_runtime/frame_pacer.hpp =====
class FramePacer {
public:
  enum class Mode { Paced, Standard, Vrr };
  FrameClock beginFrame() SE_REQUIRES(g_main);      // fence N-L; present statistics -> misses
  void endFrame(const StepPlan& next) SE_REQUIRES(g_main);   // waitable, then Paced: SDL_DelayNS(vblank - B(next)); none while Modal
  int presentInterval() const noexcept;             // 1, or 2 in HalfRate
  int frameLatency() const noexcept;                // 1 or 2 (editor default 2)
};

// ===== se_runtime/app.cpp =====
static bool SDLCALL eventFilter(void* ud, SDL_Event* e) {   // pushing thread, before SDL's watchers
  if (SDL_IsMainThread() || !isImmediateDispatch(e->type)) return true;
  static_cast<Inbox*>(ud)->push(*e); return false;            // SDL never drains on this thread
}
SDL_AppResult SDL_AppInit(void** st, int argc, char** argv) noexcept {
  *st = nullptr;
  try { auto app = App::create(argc, argv);         // std::expected<std::unique_ptr<App>, Error>
        if (!app) { se::showFatal(app.error()); return SDL_APP_FAILURE; }
        *st = app->release(); return SDL_APP_CONTINUE; }
  catch (...) { se::crash::dumpWithoutCrash("init"); return SDL_APP_FAILURE; }
}
SDL_AppResult SDL_AppIterate(void* s) noexcept {
  App& a = *static_cast<App*>(s);
  try {
    a.progress.fetch_add(1, std::memory_order_relaxed);
    a.drainInbox(); a.dialogs.drain();
    const FrameClock fc = a.pacer.beginFrame();
    a.gpu.collectUploads(); a.bg.drainCompletions(a.assetSink()); a.applyFrameStartCommands();
    const StepPlan p = a.stepper.plan(fc, a.costs.current(), a.states.simRunning(), a.debuggerAttached, a.editorPlaying());
    a.sim.tickInline(a.states, p, a.input, a.schedule, a.jobs, a.session, a.replay, a.audio);
    if (a.editor) { a.editor->ui(fc.realDelta); a.editor->applyDocumentEdits(); }
    a.renderer.frame(a.states.renderSources(p), a.jobs);    // build (may throw), then FrameRecorder
    a.audio.sweep(); a.flight.record(fc, p);
    a.pacer.endFrame(a.stepper.planNext(fc, a.costs.current()));
    return a.quit.readyToExit() ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;   // only after the Durable drain
  } catch (...) { return a.onCallbackFault("iterate"); }
}
void SDL_AppQuit(void* s, SDL_AppResult r) noexcept {
  std::unique_ptr<App> a{static_cast<App*>(s)};     // null after a failed init
  if (a) a->shutdown(r); else se::log::shutdownLast();
  se::terminateIfRelease();                          // release: end the process after the final flush
}
```

## Measurements

### Busy time at the targets (single paced loop, 120 Hz sim, 59.9 Hz display)

**What:** Busy time at the targets (single paced loop, 120 Hz sim, 59.9 Hz display)

**Result:** 5.9% of one core at N=5k, 11.2% at 10k, 23.0% at 20k (10,202 visible sprites); 299/299 frames, 0 hitches. Two-thread mode at 20k: sim 21.6%, render 3.5%.

**Implication:** One core leaves 77-94% of each frame free, and a second owner thread would idle about 96% of the time. Ship W = 0.

### Tick cost scaling (one core)

**What:** Tick cost scaling (one core)

**Result:** 0.40 / 0.79 / 1.63 / 4.20 ms p50 at N = 5k / 10k / 20k / 50k (p99 0.70 / 1.08 / 2.13 / 5.03).

**Implication:** Linear. Computed from it: 59.9 Hz holds to about N=85k.

### Render pass (interpolate, cull, sort, vertices)

**What:** Render pass (interpolate, cull, sort, vertices)

**Result:** 0.118 / 0.258 / 0.532 / 1.409 ms p50 for 2,602 / 5,129 / 10,202 / 25,456 visible sprites, the same in every mode.

**Implication:** The 10k-sprite target costs about 0.5 ms, so RenderList forks stay disabled.

### Input to present, equal pacing, 59.9 Hz, N=10k (simulated vblank)

**What:** Input to present, equal pacing, 59.9 Hz, N=10k (simulated vblank)

**Result:** Single 24.00 / 32.15 ms p50/p99; two threads 29.60 / 40.45 ms. Fully past interpolation: 24.63 vs 37.93 ms p50.

**Implication:** A second thread costs 5.6 ms at p50, and 13.3 ms until the full effect shows.

### Late latch 12 ms after vblank, N=10k

**What:** Late latch 12 ms after vblank, N=10k

**Result:** Single 11.93 / 20.18 ms; two threads 17.63 / 28.56 ms; 0 hitches in both.

**Implication:** Late latching recovers most latency in both, but a 5.7 ms gap remains.

### 144 Hz display, N=10k

**What:** 144 Hz display, N=10k

**Result:** Single 10.69 / 20.03 ms; two threads 14.94 / 21.47 ms; 720/720 frames in both.

**Implication:** 4.25 ms faster at the 144 Hz target.

### 25 ms physics spike every 2 s, 59.9 Hz

**What:** 25 ms physics spike every 2 s, 59.9 Hz

**Result:** Single: 2 hitches per run at N=5k-20k (frame max 33.3-33.9 ms). Threaded: 0 spike hitches (max at most 17.6 ms). Both: 2 motion stalls and 2 jumps per run.

**Implication:** A sim thread keeps cadence but not world motion. Keep spike sources off the tick.

### Catch-up lock-in at N=50k after a 25 ms spike

**What:** Catch-up lock-in at N=50k after a 25 ms spike

**Result:** 8-step cap: 115 and 113 hitches, frame p50 33.2 ms. 3-step cap: 2 hitches, 6 catch-up ticks.

**Implication:** The policy failed, not the thread count. The budget cap gives k_max = 2 and 1 hitch per spike (computed).

### Fixed late latch, 59.94 Hz, N=30k, at most 3 steps, 40 s (red team; <scratchpad>/redteam-perf/latch_drift_30k.txt)

**What:** Fixed late latch, 59.94 Hz, N=30k, at most 3 steps, 40 s (red team; <scratchpad>/redteam-perf/latch_drift_30k.txt)

**Result:** Latch 8.38 ms (sized from the 2-tick p95): the first 3-tick drift frame at 7.66 s, then 969 hitches, 1426/2397 frames, 975 ticks dropped, p50 32.1 ms. Latch 5.78 ms: 0 hitches, 2397/2397 frames, p50 16.2 ms. Tick p50 2.59 ms.

**Implication:** The latch must be planned from the next frame's tick count. Locked cadence removes the drift frame at 59.94 and 119.88 Hz.

### 144 Hz display, N=50k, no forks (rig; red team runs in <scratchpad>/redteam-perf/hz_runs_all.txt)

**What:** 144 Hz display, N=50k, no forks (rig; red team runs in <scratchpad>/redteam-perf/hz_runs_all.txt)

**Result:** 8-step loop: 17 hitches, 703/720 frames. Two threads: 0 hitches. One tick per frame: 17 and 5 hitches in two runs. N=40k and N=35k with one tick per frame: 0 hitches. 165 Hz at N=50k: 312 and 285 hitches; 165 Hz at N=20k with at most 2 steps: 0.

**Implication:** The computed claim that k_max = 1 fixes this case was wrong: misses come from tick-plus-render variance near 80% load. At 10x the target this settles in HalfRate (72 Hz) unless forks are enabled.

### Fork-join speedup on the tick

**What:** Fork-join speedup on the tick

**Result:** N=50k: 4.20 to 1.87 / 1.36 / 1.18 ms with 2 / 4 / 6 workers. N=5k: 0.398 to 0.134 ms. Narrowphase 3426 to 542 us. The serial sort is about 35% of the 6-worker tick.

**Implication:** Fork-join is the parallelism that pays, and the only cure for a tick longer than dt.

### Fork-join overhead (selftest, 50 us chunks)

**What:** Fork-join overhead (selftest, 50 us chunks)

**Result:** 55-65 us p50 back to back, 67-75 us p50 parked; p99 107-144 us; max 398 us.

**Implication:** 5-25 us per fork. Chunks need at least 10 us of work, with 4 or more per participant.

### Fork-join tail at 4 workers (rig matrix, <scratchpad>/mt-proto/results.txt)

**What:** Fork-join tail at 4 workers (rig matrix, <scratchpad>/mt-proto/results.txt)

**Result:** N=20k: forked tick p50 0.53 ms, max 29.2 ms. N=5k: p50 0.146, max 8.98 ms. Single loop at N=20k: max 2.26 ms. Other threads woke 18-27 ms late in the same runs.

**Implication:** Gate sites on p99.9 and max from a soak, not medians, and fall back inline after a slow join.

### Small phase in parallel

**What:** Small phase in parallel

**Result:** Integrate+bin: 18.9 us with workers against 11.3 us serial at N=5k.

**Implication:** Every fork site must earn its place in ForkStats.

### Determinism across worker counts

**What:** Determinism across worker counts

**Result:** Identical hashes after 240 ticks for 0, 2, 4 and 6 workers: 628deeea45180d2e at N=10k, 833864191b0999b5 at N=50k.

**Implication:** W = 0 is a bit-identical debug mode.

### Reading the live World from another thread (racy control)

**What:** Reading the live World from another thread (racy control)

**Result:** 36 of 299 frames at N=10k and 203 of 299 at N=50k were read during a tick.

**Implication:** Today's pattern races in practice; a single owner removes it.

### render-optimization triple buffer

**What:** render-optimization triple buffer

**Result:** 3 torn of 406 frames under an injected 9-16 ms reader stall, against 0 for a single-exchange buffer.

**Implication:** Never reuse the branch's protocol.

### Branch-style lock (exclusive per tick, shared per frame)

**What:** Branch-style lock (exclusive per tick, shared per frame)

**Result:** N=10k with a spike: render lock wait max 26.8 ms, 2 hitches. N=50k: wait p50 0.51 ms, p99 4.65 ms.

**Implication:** A locked shared World buys nothing over a single thread.

### Snapshot handoff cost (escalation path)

**What:** Snapshot handoff cost (escalation path)

**Result:** Extract 6.4-66.4 us p50 for 112 KB-1.1 MB; publish 10-20 ns; acquire 30-200 ns cross-core; interp+cull 70.2 vs 71.2 us.

**Implication:** Escalation's cost is latency and machinery, not CPU.

### High-resolution waitable timer lateness

**What:** High-resolution waitable timer lateness

**Result:** 195-310 us p50, 519-659 us p99, max 995 us in the selftest; up to 940 us p99 in runs. A 200 us spin cuts p50 to 71 us, but p99 stays at 335 us.

**Implication:** The Paced margin must be at least about 1 ms (m = 1.5 ms); never spin.

### Cooperative shutdown

**What:** Cooperative shutdown

**Result:** A stop 10 ms into a 25 ms spike joined in 14.1-15.4 ms over 25 runs; pool join 0.05-0.30 ms; no hang in 75 runs.

**Implication:** Shutdown is bounded by the longest uninterruptible work, so lane steps are bounded too.

### Unexplained wake stalls

**What:** Unexplained wake stalls

**Result:** In 5 of 29 pool runs, threads woke 12-29 ms late at the same instant; 0 of 28 two-thread runs.

**Implication:** Trace with ETW/WPR before enabling any site by default.

### Debug versus Release, 7700X, MSVC 14.51 (red team; <scratchpad>/redteam-perf/entt_results.txt and log_debug_vs_release.txt)

**What:** Debug versus Release, 7700X, MSVC 14.51 (red team; <scratchpad>/redteam-perf/entt_results.txt and log_debug_vs_release.txt)

**Result:** EnTT views 0.040 to 2.76 ms at 5k (69x) and 0.178 to 11.2 ms at 20k (63x). reg.get sweep 2.0 to 224 ms (111x). Flat arrays 3.6-4.7x. Rig tick 0.42 to 1.27 ms (3.0x); render 0.12 to 0.69 ms (5.7x).

**Implication:** An /Od editor cannot run 120 Hz Play in real time once rules use EnTT lookups. The Dev preset is the default, and flip condition 3 is judged in Dev.

### Game of Life replica at 1920x1080 (judge-measured)

**What:** Game of Life replica at 1920x1080 (judge-measured)

**Result:** 6.46-6.62 ms p50 and 10.1 ms max serial; 1.32 ms with 7 workers, with identical results.

**Implication:** The first site expected to qualify. A sim thread alone would not fix it.

### Owner's machine topology (judge-verified)

**What:** Owner's machine topology (judge-verified)

**Result:** One unified 32 MB L3; 8 cores, 16 logical processors.

**Implication:** W_max = 7. Multi-CCD and hybrid parts are untested.

## Changes To Cost Free End State

### Runtime and main loop

**Area:** Runtime and main loop

**Before:** One thread touches world state; the only other thread is miniaudio's; a fixed-step accumulator with a 0.25 s clamp and at most 8 steps

**After:** The same single owner, driven by noexcept SDL main callbacks with an event filter and inbox and a two-phase quit. Adds fork-join workers (W = 0 shipped), one background lane, a log worker, a watchdog, an input sampler and, in Windows Paced mode, a vblank sampler. The cap becomes the budget-sized cap with Overloaded, Locked or Free cadence and next-frame planning.

### Frame pacing

**Area:** Frame pacing

**Before:** Present paced by the present mode or a precise sleep

**After:** A FramePacer with Paced (waitable, then a sleep to the window output's predicted vblank minus B; misses from present statistics), VRR and Standard modes; frame latency 1 or 2; HalfRate with hysteresis; no spin. Needs a small vendored SDL patch.

### Input

**Area:** Input

**Before:** Each step consumes one InputFrame: held state plus edges since the previous step

**After:** Edges are timestamped (raw keyboard in runtime builds, pads sampled at 1 kHz) and placed on the tick whose window contains them; 6-12 tick buffer window; the source is recorded.

### World and systems

**Area:** World and systems

**Before:** Systems are small structs with run(World&)

**After:** World& or a ParallelView with declared read/write sets; structural calls assert main and not-in-fork; pre-created storages; ENTT_USE_ATOMIC; spawn finalize in Cleanup; derived runtime Guids; GameState changes at tick boundaries.

### Rendering

**Area:** Rendering

**Before:** Headless RenderList builder reading a const World

**After:** Reads a RenderSource. Recording is a noexcept transaction with a submit guard; a retire queue, per-frame texture resolution, an upload queue independent of presenting, GPU timestamps and a visual-event list.

### Editor

**Area:** Editor

**Before:** EditDocument never stepped; Play serializes into a fresh World; Stop discards it

**After:** Adds a SessionLog, asset freeze during Play, a recovery file per Play, idle autosave, Durable saves and two-phase quit, frozen faulted sessions, dialog mailboxes, handle-only panels, idle throttling and a Schedule panel.

### Assets

**Area:** Assets

**Before:** Typed stores and importers (PNG through stb_image)

**After:** Lane jobs in about 2 ms steps with Urgent/Background classes, strands, revisions and Durable writes; LZ4 atlas pages; residency on upload fence; stb_image at import only.

### Audio

**Area:** Audio

**Before:** miniaudio; AudioEmitters and audio events drained after the step

**After:** Voices hold PCM by refcount; sounds start at tick anchor + D; late gameplay sounds play; null device on init failure.

### Tests and CI

**Area:** Tests and CI

**Before:** Catch2 determinism and round trips; GitHub Actions with ASan/UBSan and clang-tidy

**After:** Adds TSan with chaos and pile-ups; hashes across W, seeds, 30-160 Hz clocks with transitions and logged edits, and FMA3 on/off; quit, NULL-acquire, frame-transaction, AppInit-failure, inbox and dialog tests; banned-API checks; se_bench in Release and Dev; a PresentMon harness.

### Profiling and diagnostics

**Area:** Profiling and diagnostics

**Before:** Tracy on demand; per-system ring buffer panel

**After:** Two Tracy frame sets and GPU zones, ForkStats with tails, the sim tick on every log line, breadcrumbs, log rings, a flight recorder, and out-of-process crash and hang dumps.

### ImGui

**Area:** ImGui

**Before:** Tagged docking release with first-party backends

**After:** GImGui thread_local; texture IDs resolved from handles every frame.

### Thread placement

**Area:** Thread placement

**Before:** Not specified

**After:** Main, workers and samplers HighQoS with CPU sets (main on a P-core; E-cores for claim-based sites); lane QoS per job; log worker at default QoS.

### Quit and durability

**Area:** Quit and durability

**Before:** App::requestQuit quits programmatically; teardown is RAII

**After:** Two-phase quit with Durable saves; deadline after the drain; synchronous recovery on session end; release builds terminate after the final flush.

### Build presets and dependencies

**Area:** Build presets and dependencies

**Before:** MSVC debug/release/ASan, clang-cl, GCC/Clang; SDL3 3.4.x vendored

**After:** Adds a Dev preset as the editor default; SDL3 carries a small patch; Crashpad is added; PresentMon is used as a tool.

### Escalation seam

**Area:** Escalation seam

**Before:** None

**After:** SimDriver::tickInline, RenderSource, InputFrames by value and the SessionLog let the dedicated-threads driver replace the tick if a flip condition is measured.

## Dependencies

### SDL3

**Name:** SDL3

**Decision:** Pin a stable 3.4.x (re-check when pinning), built static, with SDL_MAIN_USE_CALLBACKS. A small vendored patch, offered upstream, adds the DXGI frame-latency waitable and SetMaximumFrameLatency, present statistics, per-command-buffer timestamps, native handles, a device-removed query and VK_KHR_present_id/present_wait. CI also builds unpatched, falling back to Standard pacing.

**Why:** Main callbacks keep iterating in the modal loop, and SDL provides SDL_DelayNS, SDL_IsMainThread and SDL_GPU. The patch supplies the presentation signals the red team showed SDL_GPU lacks (P1, P2, P7).

### Dear ImGui (docking) with imgui_impl_sdl3 and imgui_impl_sdlgpu3

**Name:** Dear ImGui (docking) with imgui_impl_sdl3 and imgui_impl_sdlgpu3

**Decision:** One context on main; GImGui thread_local in imconfig.h; texture IDs resolved from handles every frame

**Why:** A stray off-main call crashes at once (dep/imgui-1.92.5/imgui.cpp:1408-1414), and a raw SDL_GPUTexture* never outlives its frame (imgui_impl_sdlgpu3.cpp:5, 288-290).

### EnTT 3.16.0

**Name:** EnTT 3.16.0

**Decision:** Keep; define ENTT_USE_ATOMIC; pre-create every storage

**Why:** Single-owner structural changes with fork-join reads and disjoint writes are the use EnTT documents as safe (dep/entt-3.16.0/docs/md/entity.md:2333-2373).

### Threading primitives

**Name:** Threading primitives

**Decision:** std only (jthread, stop_token, atomic wait, binary_semaphore); first-party ForkJoinPool, BackgroundLane and SPSC rings; enkiTS as a fallback

**Why:** Small and auditable: fixed chunking, the caller runs chunks, exceptions propagate.

### Win32 platform APIs

**Name:** Win32 platform APIs

**Decision:** dxgi (IDXGIOutput::WaitForVBlank), comctl32 (SetWindowSubclass), CPU sets, SetThreadInformation QoS, QueryUnbiasedInterruptTime, WM_POWERBROADCAST and WM_QUERYENDSESSION handling

**Why:** They give per-output vblanks, modal and session-end detection, P-core placement and sleep-proof hang timing.

### Crashpad

**Name:** Crashpad

**Decision:** Out-of-process crash and hang dumps on Windows and Linux, pinned and vendored. Built via the CMake-buildable fork used by sentry-native (assumption, to verify).

**Why:** In-process MiniDumpWriteDump is unsafe in exactly the failures it is for, DbgHelp is single-threaded, and Linux has no equivalent (C11, P11).

### miniaudio

**Name:** miniaudio

**Decision:** Main owns ma_engine; voices hold PCM by refcount; null-backend fallback

**Why:** One owner for an API with undocumented thread safety, and no use-after-free at Stop or hot reload.

### spdlog

**Name:** spdlog

**Decision:** Sinks and formatting only, used synchronously by the engine's log worker, which drains per-thread SPSC rings; shutdown last

**Why:** Its async queue's mutex would invert priority with an EcoQoS worker (C15), and the queue is lost at a crash (P12).

### Tracy

**Name:** Tracy

**Decision:** On demand: named threads, two frame sets, GPU zones via the patch's native handles, TRACY_DBGHELP_LOCK

**Why:** Shows serial against parallel time, fork tails and pacing in one timeline.

### Catch2 v3, Clang TSan, -Wthread-safety, ASan/UBSan, PresentMon

**Name:** Catch2 v3, Clang TSan, -Wthread-safety, ASan/UBSan, PresentMon

**Decision:** CI on Linux Clang, clang-cl and MSVC; PresentMon in the latency harness

**Why:** Proves determinism and race-freedom, and measures displayed time instead of fence time.

### LZ4

**Name:** LZ4

**Decision:** Block format for atlas pages cooked at import

**Why:** Decoding splits into steps of about 2 ms, which bounds joins and loads.

### Box2D v3 (conditional)

**Name:** Box2D v3 (conditional)

**Decision:** Only if rigid bodies are needed: one pinned version, b2World_Step on main, enqueue/finish served by a TaskSet on P-core workers

**Why:** Its solver needs concurrent tasks, which the single-slot fork cannot provide (C20).

### Not adopted

**Name:** Not adopted

**Decision:** enkiTS by default, imgui_club threaded rendering, atomic<shared_ptr>, job graphs or fibers, a snapshot ring, in-process minidumps, spdlog's async queue

**Why:** No render thread, task graph or cross-thread snapshot needs them, and in-process dumps and async queues fail under stress.

## Build Order

### World, Schedule, SimClock, InputFrame, SessionLog and a pure FixedStepper with Locked/Free cadence, the budgeted cap and next-frame planning; deterministic trig. W = 0, no window.

**Step:** 1. Headless single-thread kernel

**Goal:** World, Schedule, SimClock, InputFrame, SessionLog and a pure FixedStepper with Locked/Free cadence, the budgeted cap and next-frame planning; deterministic trig. W = 0, no window.

**Exit Criteria:** Golden hashes identical under 30/60/144/160 Hz clocks, including a GameOver transition and logged edits. Stepper tests: N=50k plus a spike gives at most 1 hitch per spike predicted; no 3-tick frames at 59.94 Hz Locked; N=50k at 144 Hz settles in HalfRate. FMA3 on/off hashes match. ENTT_USE_ATOMIC defined, storages pre-created, structural asserts in place.

### MainThreadRole, banned-include/API and link-graph checks, breadcrumbs, log rings and the log worker, Crashpad with registered streams.

**Step:** 2. Thread roles, contract checks and crash capture

**Goal:** MainThreadRole, banned-include/API and link-graph checks, breadcrumbs, log rings and the log worker, Crashpad with registered streams.

**Exit Criteria:** CI fails on each planted violation. A crash test and a watchdog-requested hang dump both show breadcrumbs, log rings and the InputFrame ring, written out of process.

### noexcept callbacks, SDL_AppQuit that accepts NULL, event filter and inbox, window subclass (Modal, session end, power), two-phase quit skeleton, idle and Suspended rates, QoS tags.

**Step:** 3. Platform loop on SDL main callbacks

**Goal:** noexcept callbacks, SDL_AppQuit that accepts NULL, event filter and inbox, window subclass (Modal, session end, power), two-phase quit skeleton, idle and Suspended rates, QoS tags.

**Exit Criteria:** A worker-pushed LOW_MEMORY reaches main without a queue drain on the worker. Ticks continue during a drag with Modal set. NULL acquires on a visible window never suspend. AppInit failure at each step is clean under ASan.

### Waitable, present statistics, timestamps, native handles, device-lost query, present_wait; Paced, Standard and VRR; vblank sampler; adaptive latency; HalfRate; PresentMon harness.

**Step:** 4. SDL presentation patch and FramePacer

**Goal:** Waitable, present statistics, timestamps, native handles, device-lost query, present_wait; Paced, Standard and VRR; vblank sampler; adaptive latency; HalfRate; PresentMon harness.

**Exit Criteria:** Paced misses fewer than 1% of vblanks at target load on the 7700X by present statistics. An injected late frame drains within 2 frames. Two monitors pace to the window's monitor. VRR runs 120 Hz Locked. Displayed-time latency is within the 1.5-frame target.

### RenderList over RenderSource, frame transaction, retire queue, resolver, upload queue, editor viewports.

**Step:** 5. Renderer2D on main

**Goal:** RenderList over RenderSource, frame transaction, retire queue, resolver, upload queue, editor viewports.

**Exit Criteria:** 10k-sprite RenderList at most 1 ms. Throw injection at every recording step for 100 frames on D3D12 and Vulkan under ASan. A texture reloaded while minimized is correct after restore.

### Raw keyboard, input sampler, tick placement, buffer window; voices with refcounted PCM and scheduled starts.

**Step:** 6. Input placement and audio

**Goal:** Raw keyboard, input sampler, tick placement, buffer window; voices with refcounted PCM and scheduled starts.

**Exit Criteria:** Win32 stamp resolution measured. Key and pad presses 8 ms apart land on different ticks. Replays reproduce. Sounds from one frame's two ticks start 8.33 ms apart. Stop and hot reload during playback are clean under ASan.

### One thread, Urgent/Background classes, SPSC rings, strands, revisions, Durable jobs, bounded steps.

**Step:** 7. Background lane

**Goal:** One thread, Urgent/Background classes, SPSC rings, strands, revisions, Durable jobs, bounded steps.

**Exit Criteria:** Quit during a large job joins within one step. Durable saves survive quit on a throttled disk. Out-of-order completion never renames over a newer revision. Steps measure at most about 2 ms.

### Documents, commands and undo; Play with SessionLog and asset freeze; Pause/Step/Stop; Play.Faulted; recovery files; autosave; Durable quit; dialog mailboxes.

**Step:** 8. Editor on the single loop

**Goal:** Documents, commands and undo; Play with SessionLog and asset freeze; Pause/Step/Stop; Play.Faulted; recovery files; autosave; Durable quit; dialog mailboxes.

**Exit Criteria:** Stop leaves an identical document hash. Tick or render exceptions leave the editor alive and the World inspectable. A recorded editor session replays headless to the same hash. A foreign-thread dialog callback is safe, including at quit.

### Hang detection on the unbiased clock armed after the first iterate; shutdown deadline; joined last; release fast exit.

**Step:** 9. Watchdog

**Goal:** Hang detection on the unbiased clock armed after the first iterate; shutdown deadline; joined last; release fast exit.

**Exit Criteria:** An injected infinite loop yields an out-of-process dump within 6 s that replays to the hanging tick. Sleep/resume, Modal, Suspended, a debugger and a slow boot trigger nothing. CI teardown under 2 s.

### Pool, sites with read/write sets, per-chunk outputs and sinks, ForkStats with tails, chaos mode, E-core calibration; a TaskSet only if Box2D is adopted.

**Step:** 10. ForkJoinPool built, still disabled

**Goal:** Pool, sites with read/write sets, per-chunk outputs and sinks, ForkStats with tails, chaos mode, E-core calibration; a TaskSet only if Box2D is adopted.

**Exit Criteria:** TSan property tests green, including no lost wake. Hashes identical for W = 0/1/3/7 and 8 chaos seeds, including pile-ups. An ETW or Tracy trace explains or bounds the 12-29 ms stalls.

### Measure S, R and g, and editor frames at 60/144/160 Hz in Release and Dev, on the 7700X, a 4-core laptop and a 2P+8E laptop on battery, Mesa/Wayland and NVIDIA/X11.

**Step:** 11. se_bench on the real stack

**Goal:** Measure S, R and g, and editor frames at 60/144/160 Hz in Release and Dev, on the 7700X, a 4-core laptop and a 2P+8E laptop on battery, Mesa/Wayland and NVIDIA/X11.

**Exit Criteria:** Thresholds frozen and flip conditions recorded. Gates on: 144 Hz editor frame at most 6.4 ms (Release); game frame at most 50% of a 144 Hz period; overload counter 0 at the targets; the 60 s 59.94 Hz scene with 0 drift misses.

### For example the Game of Life row bands, or collision on a weak machine.

**Step:** 12. Enable fork sites only where se_bench records a miss

**Goal:** For example the Game of Life row bands, or collision on a weak machine.

**Exit Criteria:** Faster than inline at the median, overhead at most 10%, join p99.9 and max no worse than inline over a 10-minute soak on the weakest machine, identical hashes.

### A dedicated sim thread through SimDriver and SnapshotChannel, only if a flip condition is measured.

**Step:** 13. Escalation (conditional)

**Goal:** A dedicated sim thread through SimDriver and SnapshotChannel, only if a flip condition is measured.

**Exit Criteria:** Inline and threaded hashes match; TSan clean; an editor mirror for the Play panels; inline stays the default wherever it holds the frame rate.

## Tests And Tooling

- Determinism: golden replays with W in {0,1,3,7} and 8 chaos seeds, comparing a Guid-keyed hash every tick (golden files per platform and compiler).
- Frame-clock independence: the same replay, including a GameOver transition, spawns and logged editor edits, under --fixed-frame-dt at 30, 60, 144 and 160 Hz gives identical hashes.
- FMA3: the determinism suite with _set_FMA3_enable forced on and off; the math self-test hash matches the replay header.
- FixedStepper unit tests: N=50k with a spike; 59.94 Hz Locked; 144 Hz with 2 ticks due; N=50k at 144 Hz; stalls; dilation; Overloaded entry and exit; the editor floor of 0; planning before the sleep.
- ForkJoinPool property tests under TSan: each chunk runs once, the final decrement is the last access, the caller runs chunks, exceptions are rethrown, nested forks run inline, no lost wake. Chaos delays and injected late wakes.
- Fork write discipline: pile-up and edge-of-world scenes under TSan and the determinism suite; ParallelView death tests for undeclared access and World-level sink use in a fork.
- Inbox: LOW_MEMORY and TERMINATING pushed from a worker while main iterates; the worker never drains SDL's queue; regular events keep their order.
- File dialogs: a callback invoked from a foreign thread during Play and after quit begins touches no engine state.
- Contract checks: banned-include, banned-API (clocks, cmath transcendental functions) and link-graph rules; compile-fail tests (-Werror=thread-safety); death tests for the asserts; a grep for ImTextureID or SDL_GPUTexture* members in se_editor; the stray-ImGui crash test.
- Quit durability: N dirty documents on a throttled disk; bytes on disk equal each document; a failed save cancels the quit; the deadline starts after the drain.
- Shutdown stress: quit at a random frame 100 times under ASan/UBSan and TSan; no surviving threads; teardown at most 2 s.
- AppInit failure at each step, including no audio device, under ASan; SDL_AppQuit handles NULL.
- Frame transaction: throw at every recording step for 100 frames on D3D12 and Vulkan; no leaked swapchain image, no acquire hang.
- NULL acquire injection on a visible window: presentation resumes within 2 frames, with no Suspended entry.
- GPU lifetime: hot reload, asset delete, document close and viewport resize while panels show the textures; the debug wrapper never fires; uploads made while minimized appear after restore.
- Audio lifetime: Stop, scene change and hot reload while voices play, under ASan; a 10-minute minimize leaks no voices.
- Fault injection in a play tick and in a play render: Play.Faulted, editor alive, document hash unchanged, brute-force picking works.
- Hang injection: an out-of-process dump within 6 s that replays headless; sleep/resume, Modal, Suspended, a debugger and a 20 s boot do not trigger it.
- Interpolation at alpha 0.5: spawn in either component order, despawn, teleport, camera cut, attachment teleport, id reuse, pause and resume.
- se_bench: 5k colliders at 120 and 160 Hz; 10k-sprite RenderList; N=50k plus a spike at 60 Hz; N=30k for 60 s at 59.94 Hz; N=50k at 144 Hz; per-site soaks; editor frames in Release and Dev; on the 7700X, a 4-core laptop and a 2P+8E laptop on battery.
- Latency harness: SendInput with QPC stamps to PresentMon displayed time plus a photodiode, for Paced, VRR and Standard, including a two-monitor case. The engine's share must stay at most 1.5 frames mean and 2 max at 60 Hz.
- Pacing stress: an injected late GPU frame is detected and drained within 2 frames; a 60 s drag causes no Overloaded episode; HalfRate does not flap at a 1% miss load.
- Tracy: named threads, two frame sets, zones per system and fork, GPU zones, plots of ticks per frame, alpha, k_max, B, frame latency, debt and dilation.
- ETW/WPR capture of the pool wake stalls before any site is enabled by default.
- Flight recorder and crash handler: hotkey and automatic dumps contain frame records and system info; the minidump has breadcrumb and log-ring streams and the Play-start bytes.
- Runtime switches: SE_JOBS=N, SE_BG_INLINE=1, SE_CHAOS_SEED, --det-crosscheck, --fixed-frame-dt, --break-on-overload, --flight-dump.

## Risks

### The single-thread ceiling is unmeasured on the real stack: the rig has no GPU, ImGui or game rules.

**Risk:** The single-thread ceiling is unmeasured on the real stack: the rig has no GPU, ImGui or game rules.

**Mitigation:** se_bench on the 7700X and weak laptops on battery, in Release and Dev, before freezing thresholds. Evaluate the flip conditions and keep the escalation seam.

### Serial work accumulates on main over the years.

**Risk:** Serial work accumulates on main over the years.

**Mitigation:** Per-phase se_bench budgets, the Schedule panel and CI gates.

### The budget-cap and HalfRate outcomes at N=50k are computed.

**Risk:** The budget-cap and HalfRate outcomes at N=50k are computed.

**Mitigation:** Cost-model unit tests, permanent se_bench scenes, and Tracy plots of k_max and debt.

### Carrying a vendored SDL patch for years.

**Risk:** Carrying a vendored SDL patch for years.

**Mitigation:** Keep it small (estimated 500-900 lines) behind se_platform. Offer it upstream, keep CI building unpatched with the Standard fallback, and follow a rebase checklist per SDL update.

### Present statistics or the vblank sampler misbehave with mixed refresh rates, hybrid adapters or independent flip.

**Risk:** Present statistics or the vblank sampler misbehave with mixed refresh rates, hybrid adapters or independent flip.

**Mitigation:** Mismatch detection drops to Standard; a two-monitor and a hybrid-laptop test.

### Unexplained 12-29 ms wake stalls in pool runs.

**Risk:** Unexplained 12-29 ms wake stalls in pool runs.

**Mitigation:** Ship W = 0; trace with ETW and Tracy; the tail soak gate and inline fallback.

### A fork-protocol bug (the prototype's first pool had one).

**Risk:** A fork-protocol bug (the prototype's first pool had one).

**Mitigation:** Property tests, TSan chaos and enkiTS behind the same API.

### A hang freezes the editor as well as the game.

**Risk:** A hang freezes the editor as well as the game.

**Mitigation:** Durable saves, a recovery file per Play, idle autosave and out-of-process hang dumps.

### Windows event stamps too coarse for tick placement.

**Risk:** Windows event stamps too coarse for tick placement.

**Mitigation:** Raw keyboard in runtime builds, the pad sampler, and a fallback to the frame's first tick (no latency cost).

### Determinism discipline erodes as parallel systems and editor features are added.

**Risk:** Determinism discipline erodes as parallel systems and editor features are added.

**Mitigation:** CI over W, chaos seeds, clocks and FMA3; --det-crosscheck; a headless test that fails on any unlogged play-World mutation.

### Linux FIFO stalls pause the tick; Linux pacing parity is unproven.

**Risk:** Linux FIFO stalls pause the tick; Linux pacing parity is unproven.

**Mitigation:** Standard on XWayland by default, present_wait where available, paused-time accounting; a flip condition if the stalls are seen on target hardware.

### Hybrid CPUs place threads badly on battery.

**Risk:** Hybrid CPUs place threads badly on battery.

**Mitigation:** CPU sets, EfficiencyClass calibration, and measurement on a 2P+8E laptop.

### Editor work in Debug is too slow on one thread.

**Risk:** Editor work in Debug is too slow on one thread.

**Mitigation:** Dev preset by default, Dev budgets, and the editor UI floor with counted dilation.

### Scheduled audio adds about 14 ms at 60 Hz Paced.

**Risk:** Scheduled audio adds about 14 ms at 60 Hz Paced.

**Mitigation:** Tune D per mode, offer an immediate mode, and measure A/V sync.

### Crashpad adds build weight and a second process to ship.

**Risk:** Crashpad adds build weight and a second process to ship.

**Mitigation:** A pinned CMake fork with the handler shipped beside the executable. If it fails to start, logs and the flight recorder are still written.

### More engine thread roles to maintain.

**Risk:** More engine thread roles to maintain.

**Mitigation:** Each role is single-purpose with no world access, named in Tracy, and covered by the shutdown tests.

## When Not To Use This

- The real-stack post-fork tick S on the weakest supported machine falls in (T - R - m)/k < S <= dt - 0.5 ms for a shipped mode. With m = 1.5 ms this is about 4.4-7.8 ms for a 144 Hz game (R = 1 ms), 2.4-7.8 ms for a 144 Hz editor (R = 3 ms, measured in the Dev preset) and 7.1-7.8 ms at 60 Hz (computed). Then switch to the dedicated-threads design, keeping inline as the default where it holds the frame rate.
- Rampage needs a simulation cadence that survives window-thread blocking: rollback or lockstep netcode, audio-locked timing, or Linux/Wayland FIFO stalls seen on target hardware (on compositors without fifo-v1 they freeze main while the window is hidden).
- The editor must stay responsive through long or hung Play ticks, and the Dev preset, the editor UI floor, durable saves and hang dumps prove insufficient in practice.
- Spikes longer than the frame's slack occur in normal play and cannot be moved off the tick, for example synchronous streaming or scripting-VM pauses.
- 240-360 Hz displays become a target while ticks cost more than about 1.5 ms.
- The budget cap and Locked cadence still fail to remove sustained half-rate lock-in at the target loads in the real engine.
- Never the task graph at this scale. It only makes sense at sustained parallel load many times the targets, with a serial spine at or below about 15-25%, phase work far above the fork cost, and the pool stalls explained.

## Open Questions

- Real-stack S, R and g at the targets, in Release and Dev, on the 7700X, a 4-core laptop and a 2P+8E laptop on battery.
- Resolution of Win32 message timestamps, and the CPU cost and edge quality of 1 kHz SDL_UpdateGamepads polling on battery.
- The cause of the 12-29 ms wake stalls.
- Whether IDXGIOutput::WaitForVBlank tracks the window's output on hybrid laptops, and whether it blocks while the output is off.
- Whether windowed flip-model present statistics are reliable on the target drivers, and how fast the waitable drains a late frame.
- The final size of the SDL patch, its upstream acceptance, and whether a future SDL exposes these signals itself.
- VK_KHR_present_wait support on Mesa and NVIDIA, and fifo-v1 support on the target compositors.
- Whether SDL's Wayland backend reports the xdg_toplevel suspended state as OCCLUDED, and whether SDL_GPU exposes any device-lost signal.
- Whether HighQoS keeps threads on P-cores under Intel Thread Director on battery, and the cost of the lane's per-step QoS switches.
- Whether Clang thread-safety analysis treats chunk lambdas as not holding MainThreadRole on the pinned Clang.
- How long serializing a large document on main takes, for recovery files and session-end writes.
- Whether /fp:precise rules out FMA contraction as fully as -ffp-contract=off.
- Real decode and parse step times for cooked assets.
- Whether the Crashpad fork builds under the repository's presets, and whether DumpWithoutCrash hang dumps capture all threads consistently.
- The vendored ImGui folder says 1.92.5 but imgui.h reports 1.92.6 WIP; confirm the thread_local GImGui override on the pinned release.

## Caveats

- Measurements come from a synthetic rig on one machine (7700X, one CCD, Windows 11) with no GPU, driver, compositor, ImGui or game rules. Each configuration ran once for 5 s, so each p99 rests on about 3-6 samples. The red team's extra runs used the same rig binary and a Debug build of it.
- The budget cap's results are computed; the rig measured fixed caps of 8, 3 and 1.
- Latencies are to a simulated vblank; mid-screen adds T/2 (computed), and USB and display latency are excluded. The lost-refresh mechanism (P1) was inferred from DXGI semantics and SDL source, not measured on hardware.
- Determinism and golden hashes hold per platform and compiler only.
- The verdict covers the stated targets on the owner's machine; players' machines are untested.
- SDL behaviour was checked against SDL's main branch on GitHub: the off-main watcher, live resize, file dialogs, D3D12 release semantics and NULL-acquire paths. Re-check all of them when pinning a release. The SDL patch's size is an estimate.
- The five goals are proxies. This design delivers smooth frames, low latency and headroom at the targets, and marks the literal goals partly met or not met wherever they require two concurrent owners.
- Estimated figures: pump 0.1 ms, record 0.4 ms, GPU 1 ms, the audio period, lane step times, the input sampler's cost and B's non-tick terms. The Game of Life numbers come from a judge's replica.
- Crashpad's CMake build route and its hang-dump behaviour are assumptions until built.

## Frame Timeline Text

```text
Recommended design, game build at the stated targets. 120 Hz simulation in Locked cadence (exactly 2 ticks per vblank, alpha = 1), 60 Hz display, Paced mode with frame latency 1, B = 5.5 ms for 2 planned ticks.
Scale: 1 column = 0.5 ms. Rig p50s: tick 0.40 ms, RenderList 0.55 ms. Everything else is estimated.

ms           0         5         10        15        20        25        30
vblank       V                                V                                V
display      ---------------------------------==================================
vblank smp   v................................v................................v
raw input    ..e................e......................e........................
pad smp      ............p...............................p......................
main         zzzzzzzzzzzzzzzzzzzzzzPnmRSwwwwwwzzzzzzzzzzzzzzzzzzzzzzPnmRSwwwwwww
workers x0   (none: W = 0 is the shipped default at these loads)
bg lane      ..================q........##......................................
log worker   ...........................l.......................................
GPU          ..........................uGG...............................GG.....
audio dev    m~~~~~~~~~~~~~~~~~~~m~~~~~~~~~~~~~~~~~~~m~~~~~~~~~~~~~~~~~~~m~~~~~~
watchdog     ........................................w..........................

Legend
- vblank and display: V is a vblank at 0, 16.67 and 33.33 ms. '-' is frame k-1 scanning out; '=' is frame k.
- vblank smp: v is the vblank sampler returning from IDXGIOutput::WaitForVBlank and publishing a QPC stamp.
- raw input: e is a key stamped by SDL's raw-input thread.
- pad smp: the input sampler polls every 1 ms; p is a poll that saw an edge.
- main:
  - z: SDL_DelayNS to the predicted vblank minus B. It never spins.
  - P: the pump plus frame start: inbox, mailboxes, fences, present statistics, uploads, lane completions, frame-start commands and the plan.
  - n, m: two ticks, 0.40 ms each, with GameState and SessionLog entries applied at each boundary.
  - R: voice starts, HUD, and the RenderList at alpha = 1.
  - S: the noexcept recording transaction, submit, then the retire queue.
  - w: the frame-latency waitable, until the submitted frame flips.
- bg lane: '=' is four Urgent decode steps of 2 ms (HighQoS); q is the completion pushed to the SPSC ring; '#' is a Background replay append (EcoQoS).
- log worker: l is a drain of the per-thread rings.
- GPU: u is the upload command buffer; G is the frame executing.
- audio dev: m is a mix period (about 10 ms, assumed).
- watchdog: w is its once-per-second check.

Handoffs
1. Edges go to ticks by timestamp. The windows are: n -5.5 to 2.83 ms, n+1 2.83 to 11.17, n+2 11.17 to 19.5, n+3 19.5 to 27.83.
   - The key press at 1.0 ms lands on n.
   - The pad press at 6.0 ms and the key release at 9.5 ms land on n+1.
   - The key press at 21.0 ms and the pad release at 22.0 ms land on n+3.
   - In a frame that runs no tick (Free cadence only), edges wait for the next tick.
2. The decoded page goes into frame k's upload command buffer. It becomes resident when its fence has signalled, which main sees at frame k+1's start. The old texture is retired after frame k+1's submit.
3. Submit is at 13.37 ms. Frame k flips at V1, which signals the waitable and records a hit in the present statistics.
4. Frame k+1 starts at V2 - B = 27.83 ms.
5. The voices of ticks n and n+1 start 8.33 ms apart, at their anchors + D (D = B + T/2 = 13.8 ms).
No world state crosses a thread.

Latency for the release at 9.5 ms. Tick n+1 consumes it at 11.77-12.17 ms, and frame k shows it fully at alpha = 1. It is on screen at V1 = 16.67 ms (7.2 ms after the event) and reaches mid-screen at 25.0 ms (15.5 ms).
Computed mean, event to mid-screen: 8.3 (sampling) + 5.5 (B) + 8.3 (half scanout) = 22.1 ms, with no interpolation lag.
Rig comparison, to a simulated vblank: 11.9 ms p50 late-latched, against 17.6 ms for two threads paced the same way. Real-stack latency is unmeasured; the PresentMon and photodiode harness measures it.

144 Hz display (T = 6.94 ms), same 120 Hz simulation, Free cadence. Scale: 1 column = 0.25 ms. There are 5 ticks per 6 frames; this window shows frames with 1, 1 and 0 ticks. The pacer plans each frame before sleeping: B = 4.8 ms for a 1-tick frame and 4.1 ms for the 0-tick frame.
vblank       V                          V                           V                           V
main         zzzzzzzzPnnRRRSSwwwwwwwwwwwzzzzzzzzzPnnRRRSSwwwwwwwwwwwzzzzzzzzzzzPRRRSSwwwwwwwwwwwz
P is pump and frame start; nn is one tick (0.40 ms, drawn as 0.5 ms); RRR is HUD and RenderList; SS is record and submit; w is the waitable; z is the paced sleep. The 0-tick frame re-renders the last two states with a larger alpha and starts later because its planned B is smaller.

Zoom: a fork-enabled tick from the rig (N = 50k, 6 workers; phase times from selftest.txt). Scale: 1 column = 25 us.
us           0         250       500       750       1000
main         IIsssssssssssssNNNNNNNNNNNNNNNNNNNNNNaaaaa
w1..w6       II.............NNNNNNNNNNNNNNNNNNNNNN.....
- I: integrate+bin, a fork (42 us).
- s: counting sort, serial (328 us).
- N: owner-computes pushes and narrowphase per band of cells, a fork; main also runs chunks (554 us, against 3428 us serial).
- a: apply, serial (125 us).
Tick p50 is 1.07 ms with forks and 4.10 ms serial. This load is 10x the collider target; at the targets no fork site is enabled.
```

## Red Team Dispositions

### C1

**Finding Id:** C1

**Severity:** high

**Disposition:** fixed

**Change:** Quit is two-phase. QUIT, close and requestQuit set quitPending and callbacks return CONTINUE. The prompt and a progress modal run over iterates. Saves are Durable lane jobs that no stop_token or cancel can drop. SUCCESS is returned only after the drain; a failed save keeps the document dirty and aborts the quit. The 5 s deadline starts after the drain. WM_QUERYENDSESSION and TERMINATING write recovery files synchronously. Reflected in the shutdown order, lifecycle, API sketch and tests.

**Residual:** A disk that never completes keeps the modal open until the user cancels. An OS-forced end can cut the synchronous recovery write short.

### C2

**Finding Id:** C2

**Severity:** high

**Disposition:** fixed

**Change:** A retire queue runs every SDL_ReleaseGPU* after the frame's submit. ImTextureIDs come from GpuResources::imguiId(handle) each frame, and a CI grep bans storing them in se_editor. Resource-releasing commands apply at the next frame start. A debug RenderDrawData wrapper asserts no bound texture was released. DeviceLost re-resolves handles.

**Residual:** Raw pointers cached outside se_editor are caught only by review and the debug assert.

### C3

**Finding Id:** C3

**Severity:** high

**Disposition:** fixed

**Change:** NULL acquire skips the present; the command buffer is still submitted and the event counted. Suspended is entered only on MINIMIZED, HIDDEN or OCCLUDED (and the Wayland suspended state) and left on RESTORED, SHOWN or EXPOSED, with a 10 Hz fallback probe. Suspended iterates at 10 Hz, not waitevent. DeviceLost is entered only on a device-removed status from the SDL patch.

**Residual:** SDL_GPU has no public device-lost query (assumption), so this relies on the patch.

### C4

**Finding Id:** C4

**Severity:** high

**Disposition:** fixed

**Change:** The SDL file-dialog thread is in the roster. Its callback fills a refcounted DialogMailbox (never an Editor*), calls wakeMain() and returns; main drains with try_lock. Quit waits while a dialog is open. The waits-for graph now says library threads may wait on main.

**Residual:** A dialog open at an OS-forced exit ends with the process; its callback touches only its mailbox.

### C5

**Finding Id:** C5

**Severity:** medium

**Disposition:** fixed

**Change:** The RenderList, forks, vertex generation and ImGui::Render all run before any acquire. Recording is noexcept. A FrameRecorder guard ends passes, submits, never cancels after an acquire, owns the fence for the N-L wait and runs the retire queue.

**Residual:** A failure inside SDL or a driver during recording ends in terminate and a dump, not recovery.

### C6

**Finding Id:** C6

**Severity:** medium

**Disposition:** fixed

**Change:** GameState commands apply at tick boundaries inside SimDriver::tickInline, which recomputes running Worlds before every tick. The frame-clock replays include a GameOver transition.

**Residual:** Play/Pause/Step/Stop still act between frames, but the SessionLog records them by tick.

### C7

**Finding Id:** C7

**Severity:** medium

**Disposition:** fixed

**Change:** A SessionLog records every play-World mutation the Schedule did not produce, with its tick index, and these mutations apply only at tick boundaries. Gameplay assets freeze in Play. Runtime Guids derive from (parent, tick, index). Bundles carry the SessionLog and Play-start bytes.

**Residual:** Replays hold per platform and compiler only.

### C8

**Finding Id:** C8

**Severity:** medium

**Disposition:** fixed

**Change:** The lane is one thread on every machine. Writes run in strands by destination path, with unique temp names and revisions that are never renamed over a newer one. Reads use FILE_SHARE_DELETE; renames retry sharing violations for up to 2 s; replay flushes form one strand.

**Residual:** External processes can still hold files; a persistent failure leaves the document dirty with an error.

### C9

**Finding Id:** C9

**Severity:** medium

**Disposition:** fixed

**Change:** Voices hold PCM by shared_ptr, released after the end id is drained and ma_sound_uninit returns. Scope release stops voices first; hot reload feeds new voices only. The ring is sized for the voice cap, plus a sweeper, and Suspended runs at 10 Hz. Late gameplay sounds play under a voice cap; only cosmetic sounds drop.

**Residual:** A catch-up burst after a long stall can bunch several hit sounds.

### C10

**Finding Id:** C10

**Severity:** medium

**Disposition:** fixed

**Change:** Uploads have their own command buffer, submitted every iterate, presenting or not. A texture flips generation and retires the old one only when its fence signals. CPU bytes are kept until then; DeviceLost re-reads cooked files.

**Residual:** Hot-reloaded textures appear one frame later; large atlases fill over several frames but never show half-uploaded.

### C11

**Finding Id:** C11

**Severity:** medium

**Disposition:** fixed

**Change:** Crashpad writes dumps out of process; the watchdog and the filter only request them. Rings are registered streams with seqlock slots, and TRACY_DBGHELP_LOCK is defined. The watchdog arms after the first iterate (30 s boot budget) and is joined last. SDL subsystems are quit explicitly after SDL logging is redirected to stderr. Release builds terminate after the final flush.

**Residual:** A hang inside the Crashpad handler is not covered; Debug and CI builds still run CRT teardown, bounded by the harness.

### C12

**Finding Id:** C12

**Severity:** medium

**Disposition:** fixed

**Change:** All four callbacks are noexcept with catch-alls routing to Faulted or SDL_APP_FAILURE. App::create returns std::expected with RAII cleanup. *appstate is set only after a complete init, and SDL_AppQuit accepts NULL. Audio falls back to the null backend.

**Residual:** None known.

### C13

**Finding Id:** C13

**Severity:** medium

**Disposition:** fixed

**Change:** Outputs are per chunk (growable, or count-then-fill) and merged in chunk order. Sinks have per-chunk slices, and World-level sinks assert not-in-fork. The narrowphase is split into contacts per cell range and owner-computes pushes per entity range. Sites declare read/write sets that ParallelView checks. Pile-up scenes join TSan and determinism.

**Residual:** Applies once a site is enabled; a smuggled raw pointer is still invisible.

### C14

**Finding Id:** C14

**Severity:** medium

**Disposition:** mitigated

**Change:** Sites are gated on p99.9 and max from a 10-minute soak on the weakest machine, with at least 4 chunks per participant and 2 reserved cores (1 on homogeneous 8+). A join over 4x its median falls back inline for 5 s. The worker loop is load, claim, wait(old), and missed and late wakes are counted.

**Residual:** OS-level stalls still delay a join until the fallback trips; the first slow join of an episode is not avoided.

### C15

**Finding Id:** C15

**Severity:** low

**Disposition:** fixed

**Change:** Main shares no lock with EcoQoS threads: lane and log traffic use SPSC rings, and the log worker runs at default QoS and owns spdlog's sinks. The inbox and mailboxes are taken with try_lock. The Log panel formats each line once.

**Residual:** A full log ring drops records (counted).

### C16

**Finding Id:** C16

**Severity:** low

**Disposition:** fixed

**Change:** Derived structures carry a validTick. A faulted World picks by brute force over storages (about 0.1 ms, estimated) and renders on a guarded path.

**Residual:** A half-applied tick is shown and labelled.

### C17

**Finding Id:** C17

**Severity:** low

**Disposition:** fixed

**Change:** A spawn-finalize pass in Cleanup replaces on_construct, and Transform deserializes first. Pause sets prev := current once, and teleportTick propagates through Attachment. Covered by tests.

**Residual:** Visual only.

### C18

**Finding Id:** C18

**Severity:** low

**Disposition:** fixed

**Change:** SetWindowSubclass on main catches size-move and menu-loop messages and sets Modal. While Modal there are no waits, no B adaptation and no Overloaded entry, and misses do not count.

**Residual:** Modal loops that do not send these messages are not covered (assumption).

### C19

**Finding Id:** C19

**Severity:** low

**Disposition:** fixed

**Change:** An SDL event filter diverts off-main immediate events into the inbox and returns false, so SDL never drains on a foreign thread. The inbox is drained at iterate start by swap with no lock held, and main never calls SDL under that lock.

**Residual:** Immediate lifecycle events are not ordered against input events; nothing pairs with them.

### C20

**Finding Id:** C20

**Severity:** low

**Disposition:** fixed

**Change:** If Box2D is adopted, a TaskSet on the same P-core workers runs its tasks concurrently, using latches and a bounded table independent of the fork slot. The document no longer promises rigid-body scaling otherwise.

**Residual:** Box2D's solver spins its workers, which is acceptable only because it runs inside the tick with no fork active.

### P1

**Finding Id:** P1

**Severity:** high

**Disposition:** fixed

**Change:** The SDL patch adds a frame-latency waitable (SetMaximumFrameLatency 1 or 2) and present statistics on D3D12, and present_wait on Vulkan. The pacer waits on the waitable before its sleep and detects misses from statistics, so the late equilibrium is detected and drained. The harness uses PresentMon plus a photodiode.

**Residual:** Patch maintenance; unpatched builds fall back to Standard and keep the extra refresh. The mechanism is inferred, not measured on hardware.

### P2

**Finding Id:** P2

**Severity:** medium

**Disposition:** fixed

**Change:** A vblank sampler waits on the window's own output with IDXGIOutput::WaitForVBlank. The output is re-resolved on display changes, with a fallback to Standard on a period mismatch above 0.5%. A two-monitor test was added.

**Residual:** Matching outputs across adapters on hybrid laptops is unverified.

### P3

**Finding Id:** P3

**Severity:** medium

**Disposition:** fixed

**Change:** A VRR mode (user setting) presents IMMEDIATE with ALLOW_TEARING at a self-paced 120 Hz, Locked with one tick per frame, with no miss, skip or halving logic.

**Residual:** No OS query for VRR state, so it is opt-in; frames tear outside the VRR range.

### P4

**Finding Id:** P4

**Severity:** medium

**Disposition:** fixed

**Change:** Locked cadence removes the 59.94/119.88 Hz drift frames (residue counted as 0.1% dilation). Free cadence plans the next frame's ticks before sleeping. The snap window is widened to at least 1 ms, or replaced by present statistics. A 60 s 59.94 Hz se_bench scene was added.

**Residual:** At 59.94 Hz the simulation runs 0.1% slow in wall time.

### P5

**Finding Id:** P5

**Severity:** medium

**Disposition:** fixed

**Change:** HalfRate is a state: enter above 1% misses over 2 s, leave after 5 s of headroom. Standard re-presents the last composite; Paced uses vblank phase. The refuted 144 Hz claim is replaced by the measurement and an se_bench gate.

**Residual:** At 10x the collider target a 144 Hz display runs at 72 Hz unless forks are enabled.

### P6

**Finding Id:** P6

**Severity:** medium

**Disposition:** fixed

**Change:** Frame latency adapts: 2 when C + g + m > T but max(C, g) + m < T, otherwise 1. The editor defaults to 2, and the Profiler shows the value.

**Residual:** Latency 2 adds one refresh where it is used.

### P7

**Finding Id:** P7

**Severity:** medium

**Disposition:** fixed

**Change:** The SDL patch adds timestamp queries and native handles for Tracy GPU zones. Without them g is unknown, and k_max holds at k_rt whenever the previous fence had not signalled.

**Residual:** Drivers without timestamps get the conservative rule.

### P8

**Finding Id:** P8

**Severity:** medium

**Disposition:** fixed

**Change:** Pool = physical cores - 2 (homogeneous 8+: - 1), cap 15. E-cores and second-CCX cores serve claim-based sites; P-cores only serve barrier-heavy ones. EfficiencyClass is calibrated at startup. se_bench runs on 2P+8E.

**Residual:** E-core throughput on battery is untested.

### P9

**Finding Id:** P9

**Severity:** medium

**Disposition:** mitigated

**Change:** A Dev preset (/O2 /Zi /Zo /Oy-, _ITERATOR_DEBUG_LEVEL=0 everywhere, asserts on, per-file pragma optimize to step) is the editor default. se_bench and the editor gate run in Dev. Editor Play's Overloaded floor is 0, so the UI keeps at least 60 Hz. Flip condition 3 is judged with Dev numbers.

**Residual:** A full /Od build still dilates the simulation at the targets. This cost is specific to one owner thread.

### P10

**Finding Id:** P10

**Severity:** medium

**Disposition:** fixed

**Change:** An input-sampler thread runs SDL_UpdateGamepads at 1 kHz (250 Hz on battery) with auto-update off, giving pad edges about 1 ms stamps. Sources are recorded. The text now states that edges in a 0-tick frame wait up to 6.9 ms.

**Residual:** A press and release within one poll still vanish; RAWINPUT/HIDAPI drivers are left to evaluate.

### P11

**Finding Id:** P11

**Severity:** medium

**Disposition:** fixed

**Change:** Crashpad dumps out of process on Windows and Linux. A hang is 5 consecutive periods on QueryUnbiasedInterruptTime or CLOCK_MONOTONIC; power suspend counts as Suspended. No DebugBreak. In-driver waits are labelled GPU or compositor stalls, and long operations post heartbeats.

**Residual:** Compositors that report neither minimize nor suspension keep rendering while hidden.

### P12

**Finding Id:** P12

**Severity:** medium

**Disposition:** fixed

**Change:** Per-thread log rings are registered as dump streams. Overrun counts are shown, repeats are rate-limited per call site, and the watchdog drains logs before a hang dump.

**Residual:** Records still in a ring at a crash are in the dump but not in the log file.

### P13

**Finding Id:** P13

**Severity:** medium

**Disposition:** fixed

**Change:** A banned-API check removes cmath transcendental functions from sim code in favour of first-party sin, cos and atan2. _set_FMA3_enable(0) at startup. CPU feature bits and a math hash go in replay headers. CI replays with FMA3 on and off.

**Residual:** Golden hashes remain per platform and compiler.

### P14

**Finding Id:** P14

**Severity:** medium

**Disposition:** mitigated

**Change:** Linux defaults to Standard on XWayland until fifo-v1 is shown on target compositors. Paced via present_wait where available. The suspended toplevel enters Suspended, and in-driver waits are labelled. No parity is claimed until measured.

**Residual:** Without present_wait, Linux is about one refresh behind Windows Paced (computed). Wayland FIFO blocking stays a flip-condition-2 risk.

### P15

**Finding Id:** P15

**Severity:** medium

**Disposition:** fixed

**Change:** An always-on flight recorder (32 B per frame, 2.8 MB for 600 s at 144 Hz) plus per-system costs on misses, dumped by hotkey, flag, automatically above 0.5% misses per minute, and in bundles, with system info. B and the extra tick use p99.5 over 4 s. Amortised systems stagger by Guid hash. The breadcrumb chunk field is 16 bits.

**Residual:** None known.

### P16

**Finding Id:** P16

**Severity:** low

**Disposition:** fixed

**Change:** Importer rule: discrete visual states last at least 2 ticks. A per-frame visual-event list goes to the RenderList, plus a 60 Hz preview toggle.

**Residual:** At 60 Hz only every second tick's discrete state is shown, by design.

### P17

**Finding Id:** P17

**Severity:** low

**Disposition:** fixed

**Change:** remaining gets its own cache line. The descriptor is copied once per generation. Large sites claim in batches. Chunks are at least 10 us with 4-8 per participant. Fork cost is re-measured with the real pool.

**Residual:** Rig fork costs came from a different protocol and stay provisional.

### P18

**Finding Id:** P18

**Severity:** low

**Disposition:** fixed

**Change:** Lane QoS is per job: Urgent work (blocking loads, imports, hot reload) at HighQoS, Background work at EcoQoS. The log worker runs at default QoS. Linux uses nice levels.

**Residual:** The cost of per-step QoS switches is unmeasured (estimated at a few us).

### P19

**Finding Id:** P19

**Severity:** low

**Disposition:** fixed

**Change:** The idle editor uses waitevent and is woken by input or wakeMain(). Minimized apps iterate at 10 Hz so loads progress. The game offers an on-battery frame cap from SDL_GetPowerInfo.

**Residual:** Animated panels keep the editor out of Idle.

## Change Log

- Two-phase quit with Durable saves; deadline after the drain; synchronous recovery on session end (C1).
- GPU lifetime: retire queue after submit, per-frame ImTextureIDs, resource-releasing edits at frame start (C2); upload queue independent of presenting (C10).
- NULL acquire is a skipped present; Suspended only on window events, at 10 Hz; DeviceLost only on device removal (C3).
- Added the SDL file-dialog thread with a refcounted mailbox; corrected the waits-for graph (C4).
- Exception-safe frame transaction (C5); noexcept callbacks and a SDL_AppQuit that accepts NULL (C12).
- GameState changes and a SessionLog at tick boundaries; asset freeze in Play; derived Guids (C6, C7).
- One lane thread with job classes, strands, revisions and Durable jobs; SPSC rings and per-thread log rings replace shared mutexes (C8, C15, P18).
- Refcounted PCM in voices; late gameplay sounds play (C9).
- Out-of-process dumps (Crashpad); watchdog on an unbiased clock, armed after the first iterate, joined last; release fast exit; crash-safe log rings (C11, P11, P12).
- Fork-join: per-chunk outputs and sinks, read/write sets, split narrowphase, tail gating with fallback, lost-wake-free loop, E-core sizing, Slot false-sharing fix, Box2D TaskSet (C13, C14, P8, P17, C20).
- Faulted-World picking and interpolation edge cases (C16, C17); Modal detection via window subclass and the event filter (C18, C19).
- Presentation: SDL patch (waitable, present statistics, timestamps, present_wait), per-output vblank sampler, VRR mode, adaptive frame latency, HalfRate hysteresis (P1-P3, P5-P7).
- Locked cadence at integer display ratios with alpha = 1; next-frame planning; p99.5 costs for B and the extra tick, p95 for overload (P4, P15).
- Gamepad sampler at 1 kHz; corrected the edge-deferral wording (P10).
- Dev preset and editor UI floor (P9); deterministic trig and _set_FMA3_enable(0) (P13); Linux defaults to Standard/XWayland (P14); flight recorder (P15); visual-event list (P16); idle and minimized behaviour (P19).
- Timeline redone for Locked 60 Hz with B = 5.5 ms, the waitable, the upload buffer and the new threads. The refuted 144 Hz claim was replaced with red-team measurements, which were also added to the measurements list.
- Flip-condition ranges made consistent across fields (m = 1.5 ms; R = 1 ms for the game, 3 ms for the editor); filled in the 10k and 20k 6-worker tick figures from the rig matrix.
