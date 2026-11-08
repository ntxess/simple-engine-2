# Dedicated sim-thread driver (build step 13)

The dedicated-threads advocate design from the 2026-11-08 workflow. The redesign builds it as SimDriver::RunThreaded + SnapshotChannel behind --sim-driver=threaded, with inline as the default. It was scored by three judges but not red-teamed.

## Name

Owned-World pipeline: dedicated 120 Hz sim thread plus a platform/present thread, immutable snapshot handoff

## Thesis

Two long-lived engine threads with exclusive ownership and no shared mutable state. The sim thread owns every running World: the runtime GameStateStack worlds and the editor's Play worlds. It steps them on absolute 120 Hz deadlines. The platform thread is SDL's main thread. It owns the windows, the event pump, input routing, Dear ImGui, the editor's edit-document Worlds, the SDL_GPU device and swapchain, every GPU resource and audio control. It renders at the display rate from immutable per-tick snapshots, which it interpolates.

Everything that crosses between the two moves as an owned value through bounded lock-free single-producer/single-consumer rings. Input events and commands go to the sim. Snapshot slots, each holding render state plus a lossless delta section, come back to the platform, and their indices are returned when the platform is done with them.

Rendering stays on the platform thread for two reasons. SDL_GPU requires the swapchain acquire, the presenting command buffer and the ImGui frame on the window thread. It is also the lowest-latency placement, because a separate render thread would add a third pipeline stage.

One enkiTS fork-join pool provides parallelism inside a tick and inside a frame. It has two priority lanes, fixed deterministic chunking, and callers that run their own chunks. The same sim code also runs inline on one thread, with identical results, for debugging, low-core machines, or as a latency mode.

Honest verdict against a single paced loop. This design meets all five goals by construction. It holds a 120 Hz sim and a 144 Hz render where the single loop falls off its frame budget, and it keeps sim cadence through window drags, compositor and vsync stalls, and UI hitches. Computed: the single loop loses 144 Hz once tick cost c plus render CPU r exceeds about 6.4 ms, which means c > 4.4 ms in the game and c > 2.4 ms in the editor.

It does not cut input latency at light load, and it costs more machinery. Computed at c = 3 ms: 4.5 ms slower mean input-to-photon at 144 Hz (19.6 vs 15.1 ms), 1.5 ms faster at 60 Hz phase-locked (25.2 vs 26.7 ms).

It is therefore the better design only if the prototype measures a heavy tick at target load, above about 1.5 ms at 60 Hz phase-locked or near the 4.4 ms cliff at 144 Hz, or if cadence isolation is valued for its own sake. Otherwise its inline mode, which is the single loop, should be the default.

## Threads

### platform (SDL main thread)

**Name:** platform (SDL main thread)

**Count:** 1

**Owns:** SDL video and gamepad subsystems (it calls SDL_Init), every window, the Win32 message queue and the event pump. The InputRouter: action map from user settings, ImGui capture and viewport-focus gating, focus loss, heartbeats. The Dear ImGui context and every editor panel. The editor's EditDocument Worlds with undo stacks and selection. The SDL_GPU device, swapchains, every GPU resource and the GPU residency table (AssetKey to texture, atlas UVs, fonts, tile chunk buffers). The RenderList master. ma_engine and every ma_sound call. The consumer end of the snapshot channel and the producer ends of the input-event and sim-command rings.

**Runs At:** Display rate (60-160 Hz) for frames, each started just in time before the next vblank. Between frames it wakes on every input message, plus a 120 Hz latch flush 0.3 ms before each sim latch for the gamepad pump and a heartbeat.

**Wakes On:** Windows: MsgWaitForMultipleObjectsEx with QS_ALLINPUT and MWMO_INPUTAVAILABLE on two handles, a CREATE_WAITABLE_TIMER_HIGH_RESOLUTION timer (next frame start or latch flush) and a wake event. SDL_WaitEventTimeout is not used: it takes milliseconds and on Windows waits in MsgWaitForMultipleObjects with no high-resolution timer (SDL 3.4.18 include/SDL3/SDL_events.h:1364, src/video/windows/SDL_windowsevents.c:2527-2533). Linux: SDL_WaitEventTimeout at 1 ms granularity. Other threads set the wake event (fault, asset ready, VRR publish). An SDL_AddEventWatch callback sets it when SDL's raw-input thread queues keys.

**Never Touches:** Any running World or its registry; the sim's clock, schedule, broadphase, RNG or input state; a snapshot slot after returning it; miniaudio's device callback; any unbounded wait on another thread (its only wait on the sim is the bounded, pumping shutdown wait).

**Why:** SDL pins window creation and queries, the event pump, swapchain claim and acquire, and the presenting command buffer to the thread that initialized video. imgui_impl_sdl3's NewFrame calls main-thread-only SDL functions (dep/imgui-1.92.5/backends/imgui_impl_sdl3.cpp:970-1014). UI and present therefore share this thread. Rendering here too avoids a third pipeline stage and any cross-thread GPU ordering.

### sim

**Name:** sim

**Count:** 1

**Owns:** Every running World: the runtime GameStateStack and its Worlds, and editor Play-session Worlds. Each World has its entt::registry, SimClock{tick, dt}, entt::dispatcher and ctx resources (broadphase scratch, DebugDraw, seeded RNG, stat schema). Also: the phased Schedule; the InputFolder (held state plus per-tick edges); the replay recorder; sim-side AssetScope data (animation clips in ticks, per-frame hitbox and hurtbox shapes, prefab templates); the write side of the snapshot slot pool.

**Runs At:** Fixed 120 Hz (configurable 120-160 Hz per session) on absolute deadlines L_k = T0 + k*dt + phi. phi is a slowly slewed display-phase offset, used only when the display period is a whole multiple of dt.

**Wakes On:** Its next deadline: a high-resolution waitable timer to the deadline minus max(0.3 ms, measured overshoot p99), then an _mm_pause spin. The spin costs about 3.6% of one core at 120 Hz (computed: 0.3/8.33). Linux uses clock_nanosleep(TIMER_ABSTIME) with lowered PR_SET_TIMERSLACK. While paused or idle it does std::atomic::wait on the command doorbell's sequence number, which a std::stop_callback rings on stop.

**Never Touches:** SDL, ImGui, any GPU object, miniaudio, the edit-document Worlds, and wall-clock time inside systems (only the driver reads the clock). It never waits on the platform thread and never blocks on a full channel: it defers, coalesces or drops by rule.

**Why:** It takes sim cadence and cost off the thread that blocks on vsync, modal drag loops and UI work. That delivers the owner's separate-cores, independent-rates and never-block-rendering goals. It is also the single owner thread EnTT requires per registry (dep/entt-3.16.0/docs/md/entity.md:2335-2341).

### worker

**Name:** worker

**Count:** Physical performance cores minus 2, clamped to 0-14: 6 on the Ryzen 7 7700X, 2 on a 4-core laptop, 0 below 3 cores (chunks then run on their caller). SMT siblings are unused by default.

**Owns:** Nothing persistent: per-worker scratch arenas reset per job, plus the chunk output slots it is assigned until the master joins.

**Runs At:** On demand: the sim lane during a tick's parallel phases, the render lane during a frame's RenderList build.

**Wakes On:** enkiTS task submission (a short spin, then a semaphore sleep).

**Never Touches:** Registry structure (create, destroy, emplace, remove, or a non-const view of a missing storage), EnTT signals, SDL, ImGui, GPU API calls, and channels. Writing into a transfer-buffer range the platform mapped beforehand is plain memory and is allowed.

**Why:** It is the only way either side can use more than one core. A single pool with lanes lets the idle side's cores help the busy side.

### asset-io

**Name:** asset-io

**Count:** 1 (EcoQoS)

**Owns:** File I/O and decoding (stb_image PNG; glaze for scene, prefab and Aseprite JSON; stb_truetype baking). Also AssetIndex builds, hot-reload timestamp polling (about 4 Hz), atomic save writes handed over by the editor (temp file, then rename), and miniaudio resource-manager jobs: jobThreadCount = 0, and this thread drains them with ma_resource_manager_process_next_job before each decode chunk.

**Runs At:** On demand. Decode work is cut into chunks of at most about 2 ms.

**Wakes On:** A request doorbell (atomic wait) or the poll timer.

**Never Touches:** Worlds, GPU calls (it hands owned pixel buffers to the platform), ImGui, SDL video.

**Why:** Keeps blocking I/O and decoding off both hot threads.

### log (spdlog async worker)

**Name:** log (spdlog async worker)

**Count:** 1 (EcoQoS)

**Owns:** Log formatting and sinks: the file sink, and an SPSC ring sink that feeds the editor Log panel.

**Runs At:** On demand.

**Wakes On:** spdlog's async queue.

**Never Touches:** Engine state.

**Why:** Logging from the sim and the workers must never block: 8192 slots with overrun_oldest.

### library-owned (not engine code)

**Name:** library-owned (not engine code)

**Count:** 1 miniaudio device thread, plus SDL internals (the SDLRawInput thread when raw keyboard or mouse input is on, gamepad backends), plus Tracy threads while a client is connected

**Owns:** Mixing (the miniaudio node graph), and raw-input polling stamped with SDL_GetTicksNS (SDL 3.4.18 src/video/windows/SDL_windowsrawinput.c:57-116).

**Runs At:** Defined by each library.

**Wakes On:** Defined by each library.

**Never Touches:** Any World or snapshot. The engine never calls ma_device start, stop or uninit from the audio callback.

**Why:** Listed so the roster and the core budget are complete.

## Frame Timeline

```text
Two 60 Hz display frames with a 120 Hz sim, game runtime, phase-locked. One column = 0.5 ms. Estimated (not measured): tick cost c = 3 ms, platform frame CPU 3 ms, GPU 1 ms, safety margin 1 ms.

ms        0       4       8       12      16      20      24      28      32
          |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |
vblank    V                                V                                 V
scanout   ---------------------------------==================================---
platform  g.........k......g.......FJ#j##P.g........k.......g........FJ#j##P....
sim       .L#J#j#S.........L#J#j#S..........L#J#j#S..........L#J#j#S............
workers   ...ss..............ss.....rr........ss...............ss.....rr........
gpu       ...............................GG................................GG...
audio dev ~~~~~~~~~~~~~~~~~~~~~~~~~a~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~a~~~~~~~~~~

Legend
- vblank and scanout: V marks vblank at 0, 16.7 and 33.3 ms. '-' is frame N-1 scanning out, '=' is frame N.
- platform:
  - k: a key message wakes the thread and an InputEvent is pushed at once (handoff 1).
  - g: latch flush 0.3 ms before each sim latch (gamepad pump and heartbeat).
  - F: just-in-time frame start at vblank - 4 ms. It pumps, receives published slots (handoff 3), returns all but the newest 3 (handoff 4), and builds one FrameView.
  - J: forks the RenderList on the render lane.
  - #: HUD, or ImGui in the editor.
  - j: joins.
  - P: budgeted upload, non-blocking acquire, record, submit and present.
- sim:
  - L: latch at 0.33, 8.67, 17.0 and 25.33 ms. It drains commands applied at this boundary (handoff 2) and folds every InputEvent stamped at or before L into InputFrame k.
  - #: the phased schedule.
  - J/j: fork-join of the physics phase on the sim lane.
  - S: snapshot extraction plus slot publish at L + 3 ms (handoff 3).
- workers: s = sim-lane chunks, r = render-lane chunks.
- gpu: G = frame execution.
- audio dev: miniaudio mixes continuously. a = audio events from the reliable section of the received slots, scheduled with ma_sound_set_start_time_in_pcm_frames at their tick anchor plus a constant delay.

Traced handoffs (computed from the chart)
- Key at 5.0 ms: pushed at 5.0, latched by tick L = 8.67, published at 11.67. Frame F = 12.67 shows that tick at alpha ~ 1, because phase lock places L + c + m at F. Submitted at 15.67, vblank at 16.67, visible at the top at 16.7 and mid-screen at 25.0. That is 20.0 ms input-to-photon for this sample.
- Key at 21.0 ms: tick L = 25.33, S at 28.33, F at 29.33, vblank at 33.33, mid-screen at 41.7. That is 20.7 ms.

Neither lane ever waits for the other. If tick 25.33 ran 6 ms late, frame 29.33 would draw tick 17.0 and hold its motion for one frame; nothing blocks, and the late slot is used by the next frame.

144 Hz display with the same 120 Hz sim, free-running and interpolated. The render delay D' = c + m = 4 ms behind the newest state.

ms        0       4       8       12      16      20      24      28      32
vblank    V             V             V             V             V            V
platform  ......F#####P.......F#####P.......F#####P.......F#####P......F#####P..
sim       .L#####S.........L#####S..........L#####S..........L#####S.........L##

There are 6 frames per 5 ticks. Every frame interpolates the bracketing pair of retained slots by their wall-time anchors, and the alpha pattern repeats every 41.7 ms.
```

## Data Flow

### InputEvent: stampNs (monotonic steady_clock at pump time), deviceNs (SDL timestamp, diagnostics only), action id, kind (press, release, axis, releaseAll, heartbeat), value.

**From:** platform

**To:** sim

**What:** InputEvent: stampNs (monotonic steady_clock at pump time), deviceNs (SDL timestamp, diagnostics only), action id, kind (press, release, axis, releaseAll, heartbeat), value.

**Mechanism:** SPSC ring of 4096 POD entries with cache-line-padded head and tail (producer store-release, consumer load-acquire). The sim peeks the front and pops only events stamped at or before L_k.

**Frequency:** Every routed input event, plus a heartbeat and a gamepad snapshot at each 120 Hz latch flush.

**Ownership Rule:** Copied by value. The sim's InputFolder is the only holder of game input state and never reads SDL. If the ring is full, the platform coalesces axis events and counts drops; it never blocks.

### SimCommand {seq, stampNs, op}. Ops: StartGame; StartPlay{session, BEVE world bytes, seed, tick rate}; Stop; Pause; Resume; Step{n}; SetTimeScale; SetWakePhase{phi}; Set/Add/RemoveComponent{Guid, type, BEVE}; Create/DestroyEntity; SpawnStress; SelectForMirror; SetDebugDraw; ScopeResident; ViewportChanged; Suspend; Quit.

**From:** platform

**To:** sim

**What:** SimCommand {seq, stampNs, op}. Ops: StartGame; StartPlay{session, BEVE world bytes, seed, tick rate}; Stop; Pause; Resume; Step{n}; SetTimeScale; SetWakePhase{phi}; Set/Add/RemoveComponent{Guid, type, BEVE}; Create/DestroyEntity; SpawnStress; SelectForMirror; SetDebugDraw; ScopeResident; ViewportChanged; Suspend; Quit.

**Mechanism:** SPSC ring of 256 move-only variants, plus a Doorbell (atomic sequence: fetch_add with release, then notify_one).

**Frequency:** On UI actions. Play-time tweaks are coalesced to one per field per frame.

**Ownership Rule:** Payload buffers move to the sim. Commands apply only at tick boundaries, in FIFO order. Each snapshot header acknowledges the last applied seq, and every applied command is recorded in the replay with its tick.

### Snapshot slot. Header: session, generation, tick, wall anchor, latch time, acked seq, tick cost, dropped publishes. Body: RenderItems, sparse id index, cameras, UI list, debug draw (when enabled), editor payload (when attached), and a reliable section (tile edits, audio events, scope releases, hierarchy deltas, session events).

**From:** sim

**To:** platform

**What:** Snapshot slot. Header: session, generation, tick, wall anchor, latch time, acked seq, tick cost, dropped publishes. Body: RenderItems, sparse id index, cameras, UI list, debug draw (when enabled), editor payload (when attached), and a reliable section (tile edits, audio events, scope releases, hierarchy deltas, session events).

**Mechanism:** A pool of 8 sim-allocated slots. The sim pushes the slot index on an SPSC 'published' ring (release) and the platform pops it (acquire).

**Frequency:** Once per tick. During a catch-up burst only the last 2 ticks carry render state. If no slot is free, the publish is skipped and the reliable section carries over to the next publish.

**Ownership Rule:** Exactly one owner per slot at any moment, and the contents are immutable while the platform holds the slot. The platform applies the reliable section of every slot it receives, in tick order, whether or not it renders that slot.

### Returned slot indices.

**From:** platform

**To:** sim

**What:** Returned slot indices.

**Mechanism:** SPSC 'returned' ring. The platform stores with release after its last read; the sim loads with acquire before reusing the slot.

**Frequency:** Every frame: all slots except the newest 3, plus every slot of an ended session.

**Ownership Rule:** A SlotLease RAII object makes holding a pointer past the return impossible in normal code. The sim reuses vector capacity, so steady state allocates nothing.

### AssetRequest{scope, AssetIds}; ScopeLoaded{scope, parsed SceneDocument, prefab templates, clips, collider shapes}; ReloadedAsset.

**From:** sim (requests) and asset-io (results)

**To:** asset-io (requests) and sim (results)

**What:** AssetRequest{scope, AssetIds}; ScopeLoaded{scope, parsed SceneDocument, prefab templates, clips, collider shapes}; ReloadedAsset.

**Mechanism:** One SPSC ring in each direction, plus doorbells.

**Frequency:** GameState and scene transitions, Play start, hot reload.

**Ownership Rule:** Decoded CPU data moves to the sim and is swapped in at a tick boundary. Replays record asset content hashes, so a reload shows up in the record.

### DecodedTexture, Atlas and Font {AssetKey, owned pixel buffer, import settings}; SoundData.

**From:** asset-io

**To:** platform

**What:** DecodedTexture, Atlas and Font {AssetKey, owned pixel buffer, import settings}; SoundData.

**Mechanism:** SPSC ring, plus the platform wake event.

**Frequency:** On load and on reload.

**Ownership Rule:** The platform uploads in a budgeted copy pass at frame start (8 MB per frame by default). Reloaded textures are swapped at a frame boundary under the same key. CPU pixels are kept or freed according to the device-loss policy.

### ScopeResident{scope}

**From:** platform

**To:** sim

**What:** ScopeResident{scope}

**Mechanism:** SimCommand ring.

**Frequency:** When every GPU upload of a scope has completed.

**Ownership Rule:** A loading GameState exits only after both ScopeLoaded and ScopeResident have arrived.

### ScopeRelease{scope, releaseTick}

**From:** sim

**To:** platform

**What:** ScopeRelease{scope, releaseTick}

**Mechanism:** Reliable section of the next published slot.

**Frequency:** When a GameState, Play session or scene drops a scope.

**Ownership Rule:** GPU objects are freed only after the platform has returned every slot with a tick below releaseTick and the fence of the last frame that drew them has signalled. SDL_ReleaseGPUTexture then defers the GPU-side release itself.

### The edit document, serialized for Play.

**From:** platform (editor)

**To:** sim

**What:** The edit document, serialized for Play.

**Mechanism:** The ComponentRegistry serializes the document World to glaze BEVE on the UI thread, and the bytes move inside StartPlay.

**Frequency:** Each Play press.

**Ownership Rule:** The document is never shared. The sim deserializes the bytes into a fresh World that it owns, so Stop cannot alter the document.

### Formatted log records.

**From:** log worker

**To:** platform (Log panel)

**What:** Formatted log records.

**Mechanism:** SPSC ring sink.

**Frequency:** Continuous.

**Ownership Rule:** The oldest records are dropped on overflow. The panel never re-reads the log file; today it does that every frame (core/util/LogStream.cpp:19-42).

### Fault {role, exception_ptr, breadcrumb: phase, system, tick}.

**From:** any thread

**To:** platform

**What:** Fault {role, exception_ptr, breadcrumb: phase, system, tick}.

**Mechanism:** A per-thread FaultBox (the exception_ptr is written, then an atomic flag is stored with release), plus the wake event.

**Frequency:** Rare.

**Ownership Rule:** The faulting side first tears down its own state on its own thread (the sim destroys the faulted World), then parks. The platform decides the outcome.

### Fork-join chunks of one phase.

**From:** sim or platform (as master)

**To:** workers

**What:** Fork-join chunks of one phase.

**Mechanism:** enkiTS ITaskSet with SetSize = the number of fixed-size chunks and MinRange 1, and one output slot per chunk. The master waits with WaitforTask and runs chunks itself.

**Frequency:** Per parallel phase: a few per tick, and one RenderList job per frame.

**Ownership Rule:** Workers write only their chunk's outputs, or entity-partitioned fields of pre-created storages. The master applies structural changes serially after the join.

## Goals

### cpu-utilization

**Goal:** cpu-utilization

**Mechanism:** The sim and platform threads run on different physical cores, placed with SetThreadSelectedCpuSets using distinct CoreIndex values from GetSystemCpuSetInformation. Workers take one remaining physical core each, and both masters run chunks of their own joins. Idle masters donate time: the sim runs render-lane chunks while it waits for its deadline, via enkiTS WaitforTask with a null task, which runs available tasks and returns when none remain (enkiTS v1.12 src/TaskScheduler.h:347-354). On hybrid CPUs these threads stay on P-cores, and asset-io and log run at EcoQoS.

**Evidence:** Roster on the 7700X: 2 dedicated threads plus 6 workers fill the 8 physical cores, and the SMT siblings are left to asset-io, log, audio and the OS. Computed sim capacity per tick: about dt minus margin, 7.3 ms of serial work, plus parallel phases spread over 7 threads. The render side has its own core plus the render lane.

**Verdict:** met

### frame-latency

**Goal:** frame-latency

**Mechanism:** The render path reads only immutable snapshots and never waits on the sim. A slow or late tick only delays the next snapshot. The frame goes ahead with the newest pair it has, holding at alpha = 1 rather than extrapolating. Frames start just in time, and the engine keeps one frame in flight with its own fence.

**Evidence:** There is no lock or wait between sim and render, and platform frame CPU time does not depend on tick cost c. Computed input-to-photon at 60 Hz phase-locked: 25.2 ms vs 26.7 ms for the single loop at c = 3 ms, and 28.2 vs 32.7 ms at c = 6 ms. At 144 Hz the single loop is faster while it can keep up: 15.1 vs 19.6 ms at c = 3 ms. But it misses 144 Hz frames once c > 4.4 ms in the game or c > 2.4 ms in the editor; this design does not miss frames until the sim itself exceeds dt.

**Verdict:** met

### independent-rates

**Goal:** independent-rates

**Mechanism:** The sim runs on absolute deadlines at a fixed tick rate. Each snapshot header carries a wall-time anchor, and the renderer interpolates with alpha = (tau - anchor_a)/(anchor_b - anchor_a). So 60 Hz, 144 Hz, VRR and editor slow motion all work without touching the sim: slow motion stretches the wall time per tick and leaves dt unchanged.

**Evidence:** Timeline: phase-locked at 60 Hz there are 2 ticks per frame, and free-running at 144 Hz there are 5 ticks per 6 frames. Test: a headless consumer runs at a random 20-200 Hz and asserts that elapsed ticks equal elapsed/dt within 1, and that the per-tick hashes equal inline mode's.

**Verdict:** met

### scalability

**Goal:** scalability

**Mechanism:** Sim cost can grow to about dt minus margin (7.3 ms at 120 Hz) of serial work per tick, plus parallel phases on the pool, without touching the platform thread's frame budget. Render-lane chunks outrank sim-lane chunks, so sim fan-out cannot starve a frame, and both masters always make progress because they run their own chunks. Beyond the sim budget, the explicit policy is bounded catch-up, then reported time dilation; a tick is never dropped.

**Evidence:** Computed: the 144 Hz render holds for any c up to about 7.3 ms. The single loop holds 144 Hz only while c + r stays below about 6.4 ms. At 60 Hz it holds while 2c + r stays below about 16 ms, which means c up to 7 ms at r = 2 ms.

**Verdict:** met

### isolation

**Goal:** isolation

**Mechanism:** Ownership by thread role is enforced at link time. Sim code lives in se_core, which may not include SDL or ImGui, and a banned-include build check guards that. The lifecycles are separate: a Play session starts and stops on the sim thread without involving the platform; the GPU device can be lost and rebuilt on the platform while the sim keeps ticking; a sim exception aborts only its Play session; and the editor document is never shared.

**Evidence:** See the lifecycle transitions StartPlay, Stop, Suspend, DeviceRecovering and PlayAborted. The data flow has no shared mutable object. World asserts its owner thread on every non-const access.

**Verdict:** met

## Issues

### sync-overhead

**Issue:** sync-overhead

**Failure Scenario:** At 10k sprites, the per-tick snapshot copy, the editor payload, ring traffic and false sharing take a visible share of the 8.33 ms tick. Separately, a fine-grained parallel_for with dozens of tiny chunks spends more on worker wake-ups than it saves.

**Mitigation:** One bulk ownership transfer per tick (a slot index), with no per-entity synchronization. Snapshots are flat, trivially copyable arrays in reused capacity, so steady state allocates nothing. A sparse id index pairs entities in O(1) without sorting. The editor payload is built only while the editor is attached, and contains only the hierarchy (on change, at most 10 Hz) and the selected entities. Fork-join is used only in phases above about 0.3 ms, with chunks of at least 256 items. Workers spin about 20 us before sleeping. Ring indices are padded to a 64-byte engine constant.

**Enforcement:** Tracy zones and plots for extraction, publish, slot occupancy and dropped publishes. A se_bench gate at 5k moving colliders and 10k sprites fails CI when extraction plus publish exceeds 5% of dt (0.42 ms).

**Residual Risk:** Extraction is estimated at 0.05-0.15 ms per tick, computed from about 0.65 MB per tick against memory bandwidth; it has not been measured. Worker wake latency on Windows has not been measured either.

### races

**Issue:** races

**Failure Scenario:** Four ways a race could creep in: a RenderItem gains a pointer into the live registry (for example const Sprite*); a panel keeps a reference into a slot after returning it; a worker calls a non-const view<T>() on a missing storage, which is a hidden structural change (dep/entt-3.16.0/src/entt/entity/registry.hpp:246-276); or someone adds a quick direct read of the play World from the editor.

**Mitigation:** There is no shared mutable state. Every object has one owner thread, and transfers happen by value or by slot ownership. RenderItem and every channel payload are trivially copyable or move-only, and hold keys and ids only. Slots are accessed through a SlotLease that exposes const views and returns the slot on destruction. The World wrapper pre-creates every registered storage and asserts its owner thread on non-const access. Workers receive const registries or partitioned write spans. ENTT_USE_ATOMIC is defined; nothing defines it today (no match in core/, app/ or any CMakeLists.txt).

**Enforcement:** Clang -Wthread-safety with role capabilities (SE_REQUIRES(kSimRole), SE_REQUIRES(kPlatformRole)) in clang-cl and in Linux CI. Runtime role asserts on every World, channel endpoint and SDL, GPU and ImGui wrapper in debug and ASan builds. A ThreadSanitizer job on Linux Clang that runs the threaded determinism suite and a 60 s headless soak. static_asserts on payload types. A banned-include check that keeps SDL and ImGui out of se_core.

**Residual Risk:** Bugs inside the small lock-free ring itself. Windows-only code paths are covered by asserts but not by TSan.

### debugging

**Issue:** debugging

**Failure Scenario:** A gameplay bug appears only in threaded runs because input lands on a different tick. Or the debugger shows the sim mid-tick while the UI shows an older snapshot. Or a breakpoint in a system freezes the sim for 30 s, and resuming triggers a catch-up storm.

**Mitigation:** Inline mode (--threads=inline) runs the same SimDriver on the platform thread through the same channels, so there is one call stack. Sim results do not depend on thread count or timing, so a replay of per-tick InputFrames and commands reproduces any threaded run inline or headless. Every slot, log line and Tracy zone carries its tick. Any gap over 250 ms, such as a debugger pause, re-bases the clock with no catch-up. Crash dumps include per-thread breadcrumbs (role, phase, tick). An editor Snapshot Viewer panel lets you browse the retained slots.

**Enforcement:** CI runs the determinism suite inline and threaded, with 0, 1 and 6 workers, and in chaos mode (seeded random 0-2 ms delays at every channel operation and fork), and asserts identical per-tick hashes. A replay round-trip test also runs in CI.

**Residual Risk:** Bugs in the threaded driver itself (pacing, phase lock, the input latch policy) do not reproduce inline. They need their own tests and Tracy captures.

### inconsistent-snapshots

**Issue:** inconsistent-snapshots

**Failure Scenario:** The renderer interpolates across a scene change. Or two different entities share a recycled slot, because EnTT's 12-bit version wraps after 4095 reuses. Or the inspector shows tick k while the viewport shows k-1 plus alpha. Or a snapshot is captured mid-tick, with destroys only half applied.

**Mitigation:** Snapshots are built on the sim thread only after the Cleanup phase has drained deferred destroys, and are published with a release store. Each header carries session, generation and tick, and a pair from different sessions or generations is never interpolated; the renderer hard-cuts to the newer one. Entities are matched on the full 32-bit id plus a per-World spawn serial. A teleport flag disables lerp; systems set it, and it is set automatically when displacement exceeds a threshold. Discrete state comes from whichever snapshot is nearer. One FrameView {S_a, S_b, alpha} per frame is shared by the viewport, picking, overlays and inspector; the inspector shows S_b's exact values labelled with their tick.

**Enforcement:** Unit tests for pairing cover spawn, despawn, teleport, slot reuse, generation change and session end. A debug assert checks S_a.tick < S_b.tick within one generation before any lerp. An overlay prints the frame's ticks and alpha.

**Residual Risk:** By design, discrete state can be up to half a tick (4.2 ms) off. A teleport below the automatic threshold that a system forgets to flag smears for one frame.

### lifecycle-shutdown

**Issue:** lifecycle-shutdown

**Failure Scenario:** The user closes the window while the sim is mid-tick, asset-io is decoding a large texture, and a held slot still names a texture. If the platform joined the sim inside the Closed handler without pumping, as core/Engine.cpp:66-75 does today, the window could ghost. GPU objects could be released under a held slot. The spdlog worker could outlive main, which is the known Windows exit deadlock. A worker exception could reach std::terminate.

**Mitigation:** An explicit state machine with ordered teardown, with every thread a jthread whose waits respect its stop_token. The platform never blocks without pumping: it waits in a bounded loop, with a 5 s watchdog that writes a diagnostic, flushes the logs and calls quick_exit. GPU releases are deferred by tick watermark plus fence. Every thread entry and every job chunk is wrapped in fault capture. spdlog::shutdown() runs before main returns.

**Enforcement:** Headless shutdown tests close the app during a tick, a catch-up burst, an asset load, Play, and Suspend. ASan catches use-after-free. An exception injected into a system must leave the editor alive with an unchanged document hash.

**Residual Risk:** A system stuck in an infinite loop cannot be stopped cooperatively, so the watchdog exits the process; autosave, not a clean shutdown, protects the editor documents.

### bottleneck-idle

**Issue:** bottleneck-idle

**Failure Scenario:** Sim-bound: c approaches 8 ms, so the sim saturates while the platform idles about 13 ms of each 60 Hz frame. Render-bound: the editor's ImGui plus 10k sprites at 144 Hz saturate the platform while the sim idles 5 ms per tick. Both masters fan out at the same moment and a frame spikes.

**Mitigation:** One pool with lanes (render HIGH, sim MED, background LOW), in which masters run their own chunks. Idle masters donate: the sim runs HIGH chunks until 0.5 ms before its deadline, and the platform runs MED chunks until 0.3 ms before its next flush or frame, while input can still wake it. Donation is on by default on AC power and off on battery, and chunks are kept under about 100 us. When the sim is overloaded, it catches up at most 8 ticks per burst; beyond 250 ms behind it dilates time (the clock is re-based, no tick is skipped) and reports this in the HUD, profiler, log and Tracy. When the render is overloaded, the frame rate drops but game speed and tick rate do not.

**Enforcement:** Tracy plots per-thread busy share, catch-up depth and dilation. The profiler panel shows sim and platform busy percentages. se_bench gates check both.

**Residual Risk:** Serial work on each side cannot be shared: game rules and structural changes on the sim, ImGui on the platform. A 4-core laptop has only 2 workers. Donation can add up to one chunk (about 100 us) of jitter to the latch.

## Snapshot And Interpolation

CONTENTS

A slot is the complete presentation state of one tick of one session.
- Header: session id; world generation (bumped on scene load, GameState switch or Play start); tick; wall anchor; latch time; last applied command sequence; tick cost; dropped-publish count. The wall anchor is the wall time the state represents, L_k + dt, because the sim runs ahead.
- items: a trivially copyable RenderItem of about 56 bytes holding:
  - entity (full 32-bit id) and spawn serial;
  - position, rotation, scale;
  - texture/atlas AssetKey, region, animation frame;
  - layer, z, tint;
  - flip and flag bits (teleported, visible).
- sparse: a map from entity index to item position, sized to the largest live index.
- cameras: position, zoom, rotation, shake offset, cut flag.
- UI list: HUD, menus and damage numbers as keyed glyph runs and quads, with strings in a slot arena.
- Debug draw: colliders, hitboxes and paths, filled only when editor overlays are on.
- Editor payload, present only when the editor is attached:
  - a hierarchy table {Guid, entity, name key, parent}, sent when the structure version changed (at most 10 Hz);
  - BEVE component blobs for the selected entities (at most 64);
  - per-system TickStats.
- Reliable section: tile edits, audio events, scope releases, hierarchy deltas and session events accumulated since the last successful publish.

Size at 10k sprites, computed: about 0.6-0.7 MB per slot, about 78 MB/s at 120 Hz, and about 5.5 MB resident for 8 slots.

BUILD

The slot is filled on the sim thread after the Cleanup phase, when deferred destroys have drained and no structural change is pending. One pass over view<Transform, SpriteRenderer> (plus Animator) writes items into the slot's reused vectors.

Above about 20k items, the pass is split across the sim lane by index ranges of the leading storage. Ranges come from the leading storage because multi-type view iterators cannot be split.

Persistent, rarely changing state (tile chunks, the hierarchy) is sent as deltas in the reliable section, never as a full copy per tick.

PUBLISH AND MEMORY ORDERS

There are two SPSC rings of uint32 slot indices: 'published' (sim to platform) and 'returned' (platform to sim). Each has an alignas(64) atomic head and tail, and each side keeps a cached copy of the other side's index.
- Push: write the element, then head.store(h+1, release).
- Pop: head.load(acquire), read the element, then tail.store(t+1, release).
- The producer checks for space with tail.load(acquire).

The sim writes the slot body before the release store of its index, so the platform's acquire load sees a complete slot. The platform returns the index only after its last read, and the sim's acquire load orders the slot's reuse after those reads. That is a happens-before edge in each direction, with no lock and no read-modify-write. It is the ownership-transfer version of the race-prone render-optimization triple buffer (render-optimization:core/util/RenderCommands2D.hpp:59-91), where load-then-store in acquireRead opens a window that two fast publishes can hit.

The sim never waits. acquireForWrite() first reclaims returned slots. If none is free, the sim skips the render part of this tick's publish and keeps the reliable deltas for the next one.

PAIRING AND INTERPOLATION

Each frame the platform:
1. Pops every published slot and applies their reliable sections in tick order.
2. Keeps the newest 3 slots and returns the rest.
3. Computes tau = F - D', where F is the frame start and D' is the render delay.
4. Picks S_a and S_b with anchor_a <= tau < anchor_b, and alpha = (tau - anchor_a)/(anchor_b - anchor_a). Because alpha uses the real anchor spacing, slow motion and dilation need no special case.

D' depends on the display:
- Free-running (for example 144 Hz): D' = c_p95 + m, about 4 ms. S_b is then always published before tau needs it.
- Phase-locked (the display period is a multiple of dt; vblank phase comes from DWM on Windows): the sim's wake offset phi is slewed by at most 0.1 ms per tick until L + c_p95 + m = F, and D' = c_p95 + m - dt. Every frame then shows the newest tick at alpha of about 1. A tick that publishes late makes that frame show the previous tick, a one-tick hold that is counted.

Starvation (tau is beyond the newest anchor):
- Hold the newest pair at alpha = 1; never extrapolate.
- Raise D' elastically to cover the gap, then decay it by at most 5% per frame, so motion resumes without a jump.
- If starvation recurs in more than 1% of frames, raise the margin m.

Per entity in S_b, look up S_a.sparse[index].
- Same full id and spawn serial, and no teleport flag: lerp position, scale and tint, and rotation along the shortest arc.
- Otherwise: draw S_b's state without lerp.

Discrete state (existence, animation frame, region, flip, layer, visibility) comes from S_a when alpha < 0.5 and from S_b otherwise.

Edge cases:
- Spawn: an entity only in S_b appears at the half-tick.
- Despawn: an entity only in S_a disappears at the half-tick.
- Teleport: a flagged entity switches at the half-tick, with no smear.
- Id reuse: EnTT's uint32 id has a 12-bit version that wraps after 4095 reuses (dep/entt-3.16.0/src/entt/entity/entity.hpp:32-39). Matching on full id plus a per-World spawn serial, assigned in World::create, means a recycled slot is never interpolated with its previous occupant. Long-lived references use Guids.
- Teleport flagging: systems set the flag on respawn, editor SetComponent with teleport, and scene load. Any displacement above a configurable threshold (default 64 px per tick) is also treated as a teleport.
- Scene and session changes: if S_a and S_b differ in session or generation, hard-cut to S_b and return the retained slots of the old generation. GPU assets of released scopes are freed only after the release-tick watermark and the frame fence have both passed.
- Camera cuts use the camera's cut flag.

## Synchronization

INVENTORY: EVERY CROSS-THREAD INTERACTION

1. SPSC rings: first-party, about 150 lines, with rigtorp-style cached indices.
   - Used for: input events (4096), sim commands (256, move-only), snapshot 'published' and 'returned' rings (16 each), asset requests and results, and the log sink.
   - Lock-free and bounded. A producer never blocks: each channel's rule says whether it coalesces, defers or counts a drop.
   - Memory orders: producer stores head with release; consumer loads head with acquire; consumer stores tail with release; producer loads tail with acquire.
2. Doorbells: a std::atomic<uint32_t> sequence. Ringing is fetch_add(release) followed by notify_one; waiting is atomic::wait(seen).
   - Waiters always wait on a monotonically increasing number, never on a flag that toggles back.
   - A std::stop_callback rings the doorbell, so every wait honours stop requests.
   - Used by the sim while paused or idle, and by asset-io.
   - MSVC implements atomic wait with WaitOnAddress; libstdc++ supports it from GCC 11.
3. Platform wake. Windows: an auto-reset event inside MsgWaitForMultipleObjectsEx, alongside the high-resolution timer and QS_ALLINPUT. Linux: SDL_PushEvent.
4. Fork-join: enkiTS task sets.
   - enkiTS counts completions internally; chunk writes happen-before the master returns from WaitforTask.
   - The master always runs chunks itself, so with 0 workers everything runs inline.
5. Immutable shared data: EngineConfig, the ComponentRegistry and the AssetIndex.
   - Each is built before any thread starts, or published as a new immutable version through a channel.
   - None is ever mutated after it is published.
6. Lifecycle: std::jthread with a stop_token. Each thread has a FaultBox: the exception_ptr is written, then an atomic flag is stored with release; the platform loads the flag with acquire after a wake.

WHAT IS ABSENT

- No mutex on any engine hot path, and no shared_mutex around a World. This replaces master's update-only m_mutex (core/Engine.cpp:181-183) and render-optimization's whole-frame shared_mutex.
- The only locks are inside libraries:
  - spdlog's async queue;
  - enkiTS's sleep path;
  - SDL_GPU's internal allocation and submit locks, which are only ever contended from the platform thread because every GPU call is made there.
- atomic<shared_ptr> is not used: it is not lock-free on MSVC or libstdc++.

COST

Computed estimate: about 10-15 uncontended atomic operations per tick (input pops, command check, slot reclaim, publish) and about 10 per frame. At 20-100 ns each, including cache-line transfers, that is about 1-2 us per tick. The real cost is the per-tick copy volume (see the sync-overhead issue).

WAITS-FOR GRAPH (acyclic, so deadlock-free by construction)

- The platform waits only on:
  - its own joins;
  - the GPU, rarely: imgui_impl_sdlgpu3 calls SDL_WaitForGPUIdle when its buffers grow (dep/imgui-1.92.5/backends/imgui_impl_sdlgpu3.cpp:136);
  - at shutdown, a bounded wait for the sim that keeps pumping events.
- The sim waits only on its own joins and its deadline.
- Workers wait on nothing.
- No thread calls SDL_RunOnMainThread with wait_complete.

GPU ORDERING

No cross-thread GPU ordering is needed, because every SDL_GPU call happens on the platform thread. That avoids the unverified question of cross-thread command-buffer visibility. Workers only write CPU memory inside a transfer buffer that the platform mapped before the fork (SDL_MapGPUTransferBuffer with cycle=true) and unmaps after the join.

## Parallel Work

POOL LAYOUT

- One enkiTS pool.
  - The platform is thread 0, because it calls Initialize.
  - The sim is a registered external task thread (numExternalTaskThreads = 1, then RegisterExternalTaskThread; enkiTS v1.12 src/TaskScheduler.h:284-289, 401).
- Lanes map to enkiTS priorities: HIGH = render, MED = sim, LOW = background (for example thumbnails).
- A master waits with WaitforTask(task, its own lane).
  - The platform therefore never runs sim chunks while waiting on a frame join.
  - The sim, waiting at MED, may run short HIGH render chunks.

INSIDE A TICK (sim lane)

Each phase is fork-joined only after a measured miss.
- Broadphase rebuild: per-chunk cell counts, a prefix sum, then a scatter in chunk order.
- Narrowphase: per-cell pair tests into per-chunk contact arrays, concatenated in chunk order, then sorted by (Guid pair, shape ids) before effects apply.
- Path and kinematic movement: gather, compute, scatter. This reuses the shape of render-optimization:core/system/WayPointSystem.cpp:100-191. Compute runs in parallel into an output array; the scatter and any emplace stay serial on the sim thread.
- Snapshot extraction: split by index ranges of the leading storage.
- Determinism hash: per-chunk partials combined in order.

EnTT rules followed:
- Every storage is pre-created.
- No structural change, signal or group mutation happens inside a parallel phase.
- Writes are partitioned by entity, or go to disjoint component types that no owning group ties together. Today core/system/WayPointSystem.cpp:14 owns MovementPattern and EntityStatus, so the new schedule uses no owning group over pools written in parallel.

Determinism:
- Chunk size is a constant of the phase (for example 256 entities), never derived from the worker count.
- Each chunk writes only its own output slot, and reductions combine in chunk order.
- Random draws use counter-based streams keyed by (seed, tick, system id, Guid).
- Results are therefore bit-identical for 0 to N workers and for inline mode, and CI asserts this.

INSIDE A FRAME (render lane)

- RenderList cull and interpolation.
- Sort keys (layer, z or y, material, entity) and a parallel radix sort.
- Vertex generation straight into the transfer buffer the platform mapped before the fork.
- ImGui stays serial.
- If Box2D is ever added, its enqueue and finish task callbacks map onto the sim lane with workerCount = pool size + 1, so it never starts a second pool.

LOAD BALANCING

- Sim-bound:
  - The sim saturates its core and its parallel phases spread over the pool.
  - It publishes render state only for the last 2 ticks of a catch-up burst, so no copy is spent on states nobody will draw.
  - The platform keeps presenting with held or elastic interpolation, and donates its idle time to MED chunks.
- Render-bound: frames slow, but the sim does not (game speed and tick rate are fixed). The sim donates its idle time to HIGH chunks.
- Both light: both threads sleep on precise timers.
  - Nothing busy-spins except the sim's 0.3 ms tail, about 3.6% of one core (computed). Today's physics loop spins a whole core (core/Engine.cpp:168-186).
  - On Intel hybrid CPUs, HIGH and MED lanes run on P-core workers only. A slow E-core chunk would hold up every join, so E-cores serve only LOW work, asset-io and log.

CATCH-UP AND SPIRAL-OF-DEATH POLICY (no silent loss of sim time)

- Behind by 1 or more ticks: run them back to back, up to 8 ticks (66.7 ms of sim time) per burst, then re-check commands and stop.
- Behind by more than 250 ms, after a process suspension, or after a debugger break: stop chasing wall time and re-base T0. This is time dilation. The game visibly slows, but every tick still runs and consumes its InputFrame, tick numbers stay consecutive, and replays are unaffected.
- Every dilation event is:
  - counted;
  - logged at most once per second, naming the top 3 systems by cost;
  - shown as a HUD or editor banner;
  - sent as a Tracy message.
- In development builds, a config switch turns sustained dilation into a debug break.

## Lifecycle

### States

- Boot
- PlatformReady
- ServicesReady
- SimIdle
- Edit (editor only: documents on the platform, sim idle)
- Running (editor Play session or runtime Gameplay; sim ticking)
- Paused (sim stopped at a tick boundary, waiting on its doorbell)
- Suspended (window minimized: no frames, no ticks)
- DeviceRecovering (platform rebuilding the GPU device; sim unaffected)
- PlayAborted (sim fault contained; editor returns to Edit)
- ShuttingDown
- Terminated

### Transitions

#### Boot → PlatformReady

**From:** Boot

**To:** PlatformReady

**Trigger:** main() starts

**Actions:** Set DPI awareness. Configure spdlog async, with all sinks set before any thread starts. Start Tracy on demand. Call SDL_Init(VIDEO|GAMEPAD) on this thread and record its role as platform. Create the window. Call SDL_CreateGPUDevice and SDL_ClaimWindowForGPUDevice. Call SDL_SetGPUAllowedFramesInFlight(1) once, since it stalls and flushes the queue. Set the present mode: VSYNC by default, MAILBOX or IMMEDIATE where the user opts in.

#### PlatformReady → ServicesReady

**From:** PlatformReady

**To:** ServicesReady

**Trigger:** Platform init succeeded

**Actions:** Read the CPU topology with GetSystemCpuSetInformation. Create the enkiTS pool; its threadStart callback sets each worker's name, QoS and CPU set. Start asset-io and build the AssetIndex. Call ma_engine_init on the platform thread with a resource manager whose jobThreadCount = 0. Create the ImGui context and the SDL3/SDLGPU3 backends (editor only). Create the InputRouter.

#### ServicesReady → SimIdle

**From:** ServicesReady

**To:** SimIdle

**Trigger:** Services ready

**Actions:** Spawn the sim jthread. It registers as an enkiTS external thread, tags itself HighQoS on its own core, names itself for Tracy, then waits on its doorbell using no CPU.

#### SimIdle → Running

**From:** SimIdle

**To:** Running

**Trigger:** Runtime start: StartGame{module start state}

**Actions:** The sim builds the GameStateStack. Its Loading state requests an AssetScope and ticks the loading screen until both ScopeLoaded (CPU data) and ScopeResident (GPU uploads) arrive. It then swaps in the World and bumps the generation.

#### SimIdle → Edit

**From:** SimIdle

**To:** Edit

**Trigger:** Editor start

**Actions:** The platform opens EditDocuments by loading and deserializing them into Worlds the platform owns. The sim stays idle.

#### Edit → Running

**From:** Edit

**To:** Running

**Trigger:** Play pressed

**Actions:** The platform serializes the document to BEVE and sends StartPlay{session, bytes, seed, rate}. The sim deserializes into a fresh World, pre-creates its storages, sets T0 at the next phase-aligned deadline and starts ticking. The UI switches to the Play view when the first slot of that session arrives.

#### Running → Paused

**From:** Running

**To:** Paused

**Trigger:** Pause command

**Actions:** The sim stops at the next tick boundary and waits on its doorbell. The renderer holds the newest pair at alpha = 1.

#### Paused → Paused

**From:** Paused

**To:** Paused

**Trigger:** Step{n}

**Actions:** The sim runs n ticks immediately, folding the input stamped up to the Step command's stamp, and publishes a slot after each tick.

#### Paused → Running

**From:** Paused

**To:** Running

**Trigger:** Resume

**Actions:** The sim re-bases T0 so paused time is never caught up, then resumes ticking.

#### Running or Paused (editor) → Edit

**From:** Running or Paused (editor)

**To:** Edit

**Trigger:** Stop

**Actions:** The sim destroys the play World on its own thread, so on_destroy signals run there. It records the replay tail, emits SessionEnded in the reliable section and returns to SimIdle. The platform returns that session's slots. The document hash is unchanged because the document was never shared.

#### Running → Suspended

**From:** Running

**To:** Suspended

**Trigger:** SDL_EVENT_WINDOW_MINIMIZED (or occluded, per policy)

**Actions:** The platform stops rendering (acquire would return a NULL texture anyway), pushes ReleaseAll and sends Suspend. The sim stops ticking. Focus loss alone does not suspend; in the runtime it makes the game push its own Pause GameState.

#### Suspended → Running

**From:** Suspended

**To:** Running

**Trigger:** Window restored

**Actions:** Send Resume. The sim re-bases T0, and the renderer re-anchors on the newest slot.

#### Running, Paused or Edit → DeviceRecovering

**From:** Running, Paused or Edit

**To:** DeviceRecovering

**Trigger:** SDL_GPU submit or acquire keeps failing with a device-removed error. SDL_GPU has no device-lost event: no such API in SDL 3.4.18 include/SDL3/SDL_gpu.h.

**Actions:** The platform destroys and recreates the device and swapchain. It re-uploads resident textures, from retained CPU pixels or by re-decoding them through asset-io, and re-initializes the ImGui renderer backend. The sim keeps ticking; the runtime may push Pause. On success, return to the previous state.

#### Running (editor Play) → PlayAborted

**From:** Running (editor Play)

**To:** PlayAborted

**Trigger:** An exception escapes a system or job on the sim

**Actions:** The tick wrapper catches it, destroys the play World on the sim thread, saves the session replay and breadcrumbs, and posts a Fault. The editor shows the system, tick and message, then returns to Edit.

#### Running (runtime) → ShuttingDown

**From:** Running (runtime)

**To:** ShuttingDown

**Trigger:** A fault in the runtime

**Actions:** Show SDL_ShowSimpleMessageBox from the platform, write a crash report that includes the replay, then follow the shutdown order.

#### any → ShuttingDown

**From:** any

**To:** ShuttingDown

**Trigger:** SDL_EVENT_QUIT, window close, or App::requestQuit from any thread

**Actions:** In the editor, first handle the unsaved-document prompt and autosave. Then follow the shutdown order.

### Shutdown Order

- 1. Platform: stop routing gameplay input, then send Quit (request_stop plus a doorbell ring) to the sim.
- 2. Platform: keep pumping events and presenting or skipping frames, polling sim.finished() with waits of at most 5 ms. A 5 s watchdog logs the sim's last breadcrumb, flushes the logs and calls quick_exit.
- 3. Sim: finish the current tick (stop_token is checked at every tick boundary and inside catch-up bursts), flush the replay, destroy all Worlds on its own thread, emit ScopeRelease for every scope, deregister from enkiTS and exit. The platform's join then returns at once.
- 4. Platform: drain the remaining published slots, apply their reliable sections (scope releases), return every slot and drop pending audio events.
- 5. Editor layer: shut down ImGui_ImplSDLGPU3 and ImGui_ImplSDL3 while the GPU device is still alive, then call ImGui::DestroyContext.
- 6. asset-io: request_stop and ring its doorbell. It cancels at chunk boundaries. Join it with a bounded wait that keeps pumping events.
- 7. Audio: call ma_engine_uninit (this stops the device thread; never from the callback), then ma_resource_manager_uninit.
- 8. Job pool: call WaitforAllAndShutdown. No submitters remain at this point.
- 9. GPU: call SDL_WaitForGPUIdle, release the renderer's pipelines, buffers and textures, call SDL_ReleaseWindowFromGPUDevice, then SDL_DestroyGPUDevice.
- 10. Platform: call SDL_DestroyWindow, then SDL_Quit.
- 11. Logging: call spdlog::shutdown() to flush the async queue and join the log worker, before main returns (Windows deadlock note). Tracy shuts down last.

### Failure Handling

EXCEPTIONS
- Every thread entry and every job chunk runs inside guardFaults(role, ...).
- A fork-join stores its first exception in the join object and rethrows it on the master after WaitforTask.
- No exception can reach std::terminate. Today one can, from core/util/TaskScheduler.cpp:135.

BY THREAD
- Sim: the tick wrapper catches the exception, marks the session faulted, destroys that World on the sim thread, saves the replay and breadcrumbs (role, phase, system, tick), posts a Fault and returns to SimIdle. In the editor the error is shown and the editor stays up; in the runtime an error dialog is shown, followed by the ordered shutdown.
- Platform: ImGui's error recovery (io.ConfigErrorRecovery, dep/imgui-1.92.5/imgui.h:2557) handles panel errors. Repeated faults trigger an autosave of documents and the ordered shutdown.
- asset-io: a failed load becomes an error result for that asset, which draws as a placeholder; it is never a fault.
- Workers hold no state of their own to tear down.

GPU DEVICE LOSS
- SDL_GPU has no device-lost API. The D3D12 backend only turns DXGI_ERROR_DEVICE_REMOVED into an error message (SDL main src/gpu/d3d12/SDL_gpu_d3d12.c:1294-1296).
- Loss is therefore detected as persistent submit or acquire failures.
- Recovery is a full rebuild of the device, swapchain, pipelines and resident textures. The sim is unaffected.
- This recovery path is an untested assumption.

HANGS
- The platform watches each thread's heartbeat counter.
- If the sim misses 2 s of ticks while not paused, the platform logs the sim's last breadcrumb and its last Tracy message.
- If the sim has not exited 5 s into shutdown, the logs are flushed and the process calls quick_exit.

OVERLOAD
- Bounded catch-up, then reported time dilation (see parallelWork).

PLATFORM STALLS
- The sim's input-liveness policy applies (see inputAndLatency).

## Editor

WHERE THINGS LIVE
- Edit documents (scenes and prefabs being edited) are Worlds owned by the platform thread, the same thread as ImGui. Their undo stacks, selection and dirty flags live there too.
- The sim owns only Play-session Worlds.
- No World is ever shared between threads.

HOW PANELS READ
Panels receive an InspectView and an EditorCommands interface.
- Edit mode:
  - The InspectView wraps the const document World directly, with zero copy.
  - The document is immutable during a frame. Commands queued by panels apply at the next frame boundary.
  - That is what makes it safe for render-lane jobs to read the const registry while ImGui builds. Const EnTT registries are thread-safe once every storage exists, and the World pre-creates all of them (dep/entt-3.16.0/docs/md/entity.md:2410-2433).
- Play mode:
  - The InspectView wraps the frame's FrameView: the hierarchy table and the selected entities' BEVE blobs from S_b.
  - Generic reflection drawers deserialize a blob into a local component value on the UI thread and draw it. This is the same drawer code as Edit mode.
  - Values are labelled with S_b's tick.

HOW EDITS TRAVEL
- Edit mode:
  - An edit is an EditorCommand: set, add or remove a component (with before/after JSON); create or destroy an entity (with the serialized entity); or a tile or path command.
  - A drag coalesces into one undo entry.
  - The headless test that fails on any document mutation made outside commands still applies.
- Play mode:
  - An edit is a SimCommand: SetComponent with a BEVE value, plus a teleport flag for transforms. It is coalesced to one per field per frame.
  - The sim applies it at the next tick boundary and acknowledges it in a later slot.
  - The widget shows its own in-progress value until the ack arrives, then shows the mirror again.
  - Play tweaks are not undoable and are discarded at Stop. The exception is an explicit 'Apply to document', which turns the mirrored values into undoable EditorCommands.

PLAY, PAUSE, STEP AND STOP ACROSS THREADS
- Play: serialize the document on the UI thread, then send StartPlay. Every Play is therefore a round trip through the serializer, as the end state requires.
- Pause: a command. The sim stops at a tick boundary, and the viewport holds the last tick.
- Step: Step{1} while paused runs exactly one tick, using the held input plus any edges stamped before the click.
- Stop: the sim discards the World.
  - The 'Stop leaves an identical document hash' rule holds by structure, because the document was never touched.
  - Today Play and Forward are two atomics over the live registry (app/editor/Editor.hpp:119-120, app/editor/Editor.cpp:126-132), raced by unlocked render-thread edits.
- 'Play game': sends StartGame with the module's GameStateStack.
- A gameplay exception aborts only the Play session (PlayAborted) and saves its replay.

PICKING
- Picking uses the frame's RenderList: the exact quads that were drawn, already interpolated and carrying entity ids. It walks them in reverse sort order and returns the topmost entity.
- In Play, the id maps to a Guid through the hierarchy table. A SelectForMirror command adds that entity's components to later payloads, one tick later.
- Collider picking uses the sim's debug-draw shapes while overlays are on.

GIZMOS
- Drawn with ImGui draw lists over the viewport, from FrameView transforms or from document transforms.
- In Edit, a drag is one coalesced, undoable command on the document.
- In Play, a drag sends SetComponent(Transform, teleport) once per frame.

PROFILER
- Sim TickStats travel in each slot's editor payload: per-system time, tick total, catch-up depth, dilation, input pump lag and extraction time.
- The UI keeps them in a 1024-entry ring next to its own FrameStats, on a shared wall-time axis.
- These are values copied out of slots, so the current race cannot happen: today the editor copies the profiler deques while the physics thread mutates them (core/util/SystemProfiler.cpp:17-19 vs app/editor/EditorSceneSystemVisitor.cpp:5).
- Tracy gives deep captures, with separate frame sets for sim ticks and display frames.

OTHER PANELS
- Hierarchy and Scene settings: read the document in Edit and the mirror in Play.
- Stress generator: in Play, sends one SpawnStress{count, prefab, seed} command, which is deterministic and recorded in the replay.
- Log: fed from the log worker's SPSC sink.
- Assets: reads the platform-side GPU table and the AssetIndex.
- Hot reload: textures swap at a frame boundary on the platform; gameplay data reaches the sim at a tick boundary.
- Multi-viewports (opt-in): secondary windows are created, rendered and presented on the platform thread inside the UI frame, as imgui_impl_sdlgpu3 requires (dep/imgui-1.92.5/backends/imgui_impl_sdlgpu3.cpp:697-734).

## Input And Latency

SAMPLING
- Keyboard and mouse messages wake the platform thread at once. It waits in MsgWaitForMultipleObjectsEx, not in SDL's millisecond wait.
- Every event gets a monotonic steady_clock stamp at pump time and is pushed to the sim. Estimated mean pump delay p is about 0.5 ms: the time until the platform finishes its current frame slice, plus about 50 us of wake-up. Extra pumps run mid-frame, after the ImGui build and before acquire.
- In runtime builds, SDL_HINT_WINDOWS_RAW_KEYBOARD is on. SDL's SDLRawInput thread then polls with SDL_GetTicksNS (SDL 3.4.18 src/video/windows/SDL_windowsrawinput.c:57-116), and an SDL_AddEventWatch callback sets the platform wake event. It is off in the editor because of hotkey and IME interplay.
- Device timestamps are kept for measurement only, never for tick assignment. SDL's Win32 message timestamps derive from millisecond message ticks whose granularity is unverified.
- Gamepads are pumped at a latch flush 0.3 ms before every tick, so each tick sees fresh pad state.

FOLDING
- InputFrame k contains all events stamped at or before L_k:
  - held bits;
  - pressed and released counts (a tap shorter than a tick is never lost);
  - latest axis values.
- A game-side buffer of the last 10 InputFrames (83 ms) serves chained actions.
- Inline mode uses a different latch rule: 'everything pumped before the tick runs'.
- Replays record the folded frames, so the latch rule does not affect them.

LIVENESS
- If no heartbeat has arrived for 3 ticks, the sim stops accepting new presses. Causes include a modal loop, a Vulkan acquire with an infinite timeout, or a message box.
- After 100 ms, held actions are released as recorded input edges, and the runtime pushes its Pause state.
- A stalled platform therefore cannot leave a character running.

LATENCY MODEL (computed; every input is an estimate, not a measurement)
Parameters:
- dt = 8.33 ms.
- Tick cost c = 3 ms. This is the p95 cost used to size D'.
- Wake and jitter margin m = 1 ms.
- Pump delay p = 0.5 ms.
- Frame start to vblank, r + g + s = 4 ms in the game and 6 ms in the editor: CPU r = 2 or 4 ms, GPU g = 1 ms, safety s = 1 ms.
- 1 frame in flight, just-in-time frame start.
- Scanout is counted to mid-screen (T/2). Monitor processing is excluded.

Formulas:
- This design, free-running: p + dt/2 (tick quantization) + c + m + T/2 (frame alignment) + (r+g+s) + T/2 (scanout).
- This design, phase-locked (T a whole multiple of dt): p + c + m + T/2 + (r+g+s) + T/2. The tick quantization folds into the frame quantization.
- Single paced loop, sim run on demand at frame start: T/2 + k*c + (r+g+s) + T/2, with k = 2 ticks budgeted per frame at 60 Hz and 1 at 144 Hz. At 144 Hz add 1.2 ms for zero-tick frames.

Means in ms (worst case in brackets), game, c = 3 ms unless noted:
                              60 Hz display                    144 Hz display
this design, phase-locked     25.2  (c=1: 23.2, c=6: 28.2)     n/a (144 is not a multiple of 120)
this design, free-running     29.3 [51.7]                      19.6 [32.2]  (c=6: 22.6)
single paced loop             26.7 [43.3]  (c=6: 32.7)         15.1 [27.8]; cannot hold 144 Hz once c > 4.4 ms (editor: 2.4 ms)
naive vsync (no JIT, +1 queued frame)  58.7                    29.5
The editor adds 2 ms to every row (r = 4 ms).

Crossovers (computed):
- 60 Hz, phase-locked: this design beats the single loop once c > 1.5 ms.
- 60 Hz, free-running: it wins once c > 5.7 ms.
- 144 Hz: it is 4.5 ms slower for as long as the single loop can hold the rate. Its advantage there is that it keeps 144 Hz when the loop cannot.
- The naive vsync row shows that pacing matters more than threading.
- The platform frame excludes sim cost, so its CPU time varies less, and the safety margin s can be tighter than in a single loop that must budget k * c_p99.

TOLERANCE FOR RAMPAGE (a design target, not a measurement)
- Mean input-to-photon at most 50 ms, excluding monitor processing.
- Worst case at most 70 ms.
- Tick-exact input placement, with no lost taps.
- Jitter within one tick, for links and cancels defined in ticks.
Reference: DisplayLag's database lists shipped fighting games at 4.2-8.0 'frames' including about 1 frame of monitor lag, recorded with a 120 fps camera; the page's frame unit is ambiguous (https://displaylag.com/video-game-input-lag-database/).

HOW THE DESIGN STAYS WITHIN IT
1. Frames start just in time, from the vblank phase.
   - DwmGetCompositionTimingInfo(NULL) provides qpcVBlank and qpcRefreshPeriod. From Windows 8.1, hwnd must be NULL (https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmgetcompositiontiminginfo).
   - An adaptive safety margin grows after any missed vblank.
2. The engine enforces its own one-frame limit. It waits on frame N-1's fence (from SDL_SubmitGPUCommandBufferAndAcquireFence) before starting frame N. SDL alone would let one more frame queue: even with SDL_SetGPUAllowedFramesInFlight(1), SDL's D3D12 backend keeps at least 2 swapchain textures and cycles its in-flight fences modulo that count (SDL 3.4.18 src/gpu/d3d12/SDL_gpu_d3d12.c:7022, 8140).
3. It uses the non-blocking acquire, which on D3D12 returns a NULL texture instead of blocking (same file, 7655).
4. It phase-locks the sim at 60 Hz and 120 Hz displays.
5. It offers two lower-latency modes:
   - MAILBOX: D3D12 presents with sync interval 0 and no tearing flag (same file, 8112-8128).
   - VRR 'render on publish': IMMEDIATE adds DXGI_PRESENT_ALLOW_TEARING. Computed at about 15 ms mean on a 144 Hz VRR panel.
6. It keeps the platform's frame CPU within budget by moving RenderList work onto render-lane jobs.

Assumption: windowed DWM composition may add up to one refresh unless the swapchain is promoted to independent flip. Not measured.

## Assets Audio And Physics

ASSETS

AssetIndex:
- Immutable. asset-io builds it by scanning the content root. The root comes from --root, then SE_ROOT, then a walk up from the executable to project.json.
- Maps a normalized path plus type to a stable 64-bit AssetKey. Key collisions are build errors.
- Hot reload publishes a new index version.

Sim side, per AssetScope (owned by GameStates and Play sessions):
- SceneDocuments and prefab templates.
- Animation clips, timed in ticks.
- Per-frame hitbox, hurtbox and pivot data from Aseprite slices, once animated characters exist.
- All of it is decoded on asset-io and moved in at a tick boundary.

Platform side, GPU residency:
- AssetKey to SDL_GPUTexture, atlas UVs, baked font atlases, and tilemap chunk buffers.
- Uploads run in a budgeted copy pass at frame start (8 MB per frame by default).
- Snapshots name assets by AssetKey only, so no pointer crosses a thread. Compare the raw const sf::Texture* in render-optimization:core/util/RenderCommands2D.hpp:26.
- A key that is not resident yet draws a placeholder. A stale key fails validation and never dangles.
- Release is ordered by tick watermark plus frame fence. SDL_ReleaseGPUTexture then defers the GPU-side release itself.
- Hot-reloaded textures keep their key and swap at a frame boundary.

Every GPU call happens on the platform thread. That avoids the undocumented thread-safety of SDL_GPU resource creation; moving uploads to asset-io with fences is a later, measured option.

AUDIO

- Library: miniaudio 0.11.x. The fetched header reports 0.11.26 (miniaudio.h:3748-3750).
- Ownership: ma_engine and every ma_sound call stay on the platform thread, the single game-side owner. The device thread only mixes.
- Resource manager: runs with jobThreadCount = 0 (miniaudio.h:10530). asset-io drains its jobs with ma_resource_manager_process_next_job (miniaudio.h:10625) before each decode chunk, so streaming never queues behind a texture decode.
- Events: sim systems emit AudioEvent{tick, soundKey, position, gain, pitch, voice policy} into the reliable section.
- Scheduling: the platform maps each event's tick anchor to engine time (ma_engine_get_time_in_pcm_frames, miniaudio.h:11370). It then calls ma_sound_set_start_time_in_pcm_frames (miniaudio.h:11469) with that time plus a constant audio delay (default D' + 1 frame). This removes the 60 Hz batching jitter a post-frame drain would add.
- Events more than 100 ms late are dropped and counted.
- The Music and Effects groups take their volumes from user settings (config/config.json:26-29).

PHYSICS AND COLLISION

First-party, on the sim thread, in the Physics phase:
- a spatial hash rebuilt every tick, with a parallel build;
- an AABB narrowphase over Body, Hitbox, Hurtbox and Trigger roles, with a layer/mask matrix;
- contacts sorted before effects;
- swept-AABB kinematic resolution.

Nothing outside the sim queries the live world. The renderer and editor read colliders from the debug-draw data in snapshots.

If Box2D v3 is added for debris:
- b2World is stepped on the sim thread, and its task callbacks go to the sim lane.
- Its events are processed right after b2World_Step.
- Its documented thread-count-independent determinism is kept by building with /fp:precise on MSVC and -ffp-contract=off on GCC and Clang.

## Determinism And Debugging

CONTRACT
Sim output depends only on these inputs: the initial World bytes, the per-tick InputFrames, the per-tick commands, the seed, the schedule and the binary. It does not depend on wall time, thread count, worker count, render rate or pacing.

RULES
- Systems read only SimClock{tick, dt}.
  - Banned-API checks reject steady_clock, system_clock and SDL_GetTicks in se_core sim code and in game systems.
  - Only SimDriver reads the clock, and only for pacing.
  - Today, effects and cooldowns read steady_clock (core/system/EventSystem.cpp:95,135).
- dt is fixed per session. Time scale changes the wall time per tick, never dt.
- Commands apply only at tick boundaries and are recorded with their tick.
- Parallel work uses fixed chunking and ordered reductions. Contacts are sorted before effects run.
- RNG uses counter-based streams keyed by (seed, tick, system, Guid), instead of today's std::random_device seeding (app/editor/Editor.cpp:939-940).
- No sim logic iterates an unordered container. Where order matters, logic sorts by Guid instead of relying on EnTT storage order.
- Floating point: /fp:precise on MSVC, -ffp-contract=off on GCC and Clang, no fast-math.
- Golden hashes are kept per platform. Equality across compilers is not promised; that is one of the stated caveats.

HASH
- A Guid-keyed projection of positions, stats and liveness is hashed every tick in tests and Play sessions.
- It is computed as per-chunk partials combined in a fixed order, and stored in the replay.

RECORD AND REPLAY
- The sim writes per-tick InputFrames, commands, asset content hashes and periodic world hashes into a BEVE replay.
- Replay mode feeds the recorded frames in place of the input ring. It runs headless as fast as possible, or paced with rendering.
- Every editor PlayAborted and every runtime crash saves its replay.

RUNNING ON ONE THREAD
- --threads=inline runs SimDriver::tickInline on the platform thread. Due ticks run on demand at frame start, which is the single-loop profile, through the same channels and slot pool. SPSC use from a single thread is trivially correct. Inline mode also ticks from the live-resize event watch, so inline runs keep stepping during Windows window drags.
- --workers=0 runs every chunk on its caller.
- --lockstep runs exactly one tick per display frame, synchronously. Use it for RenderDoc or PIX captures that pair a frame with a known tick.
- CI asserts identical per-tick hashes for inline mode, threaded mode, workers 0/1/6, and chaos mode.

TOOLING
- Tracy:
  - tracy::SetThreadName for every thread;
  - FrameMarkNamed(sim) per tick and FrameMark per display frame, as two frame sets;
  - plots for snapshot age, D', alpha, catch-up depth, dilation, input pump lag and ring occupancy;
  - zones for every phase and job chunk.
- ThreadSanitizer: a Linux Clang CI job runs se_tests plus a 60 s headless threaded soak, with the headless RenderList consumer standing in for the GPU.
- Clang thread-safety analysis: role capabilities checked with -Werror=thread-safety in clang-cl and Linux builds. On MSVC the macros compile to nothing.
- Runtime affinity asserts:
  - a thread_local role, checked by SE_ASSERT_ROLE in every SDL, SDL_GPU and ImGui wrapper (SDL_GPU itself does no thread-ownership check);
  - SDL_IsMainThread in se_platform;
  - owner-thread asserts in World and on every channel endpoint.
- Also: ASan/UBSan, the chaos scheduler, and the Snapshot Viewer panel.
- Debugger breaks: a gap over 250 ms re-bases the clock. There is no catch-up storm, and the renderer simply holds.

## Platform Fit

Each constraint from the research, and how the design meets it.

1. SDL video, windows, window queries and the event pump must run on the thread that called SDL_Init(VIDEO).
   - That is the platform thread, which is the process main thread, so the design also ports to Apple.
2. Swapchain claim, acquire, wait, and the presenting command buffer must run on the window thread.
   - All GPU recording and submission happens on the platform thread.
   - This also avoids the unverified rules for cross-thread GPU ordering and resource creation.
   - SDL does no runtime check for misuse (constraints research on src/gpu/SDL_gpu.c), so SE_ASSERT_ROLE guards every GPU wrapper.
3. ImGui is one context on one thread.
   - ImGui_ImplSDL3_NewFrame calls main-thread SDL functions (dep/imgui-1.92.5/backends/imgui_impl_sdl3.cpp:970-1014), and ImDrawData is valid only until the next NewFrame.
   - ImGui, PrepareDrawData (with its own upload command buffer) and RenderDrawData all run on the platform thread within one frame.
   - So imgui_club's threaded-rendering snapshot and texture queue are not needed.
4. Multi-viewport windows are created, rendered and presented inside the UI frame on the platform thread. They stay opt-in.
5. The SDL_Renderer fallback must run on the main thread.
   - It does: the renderer backend interface is platform-thread-only.
6. Keyboard and mouse state may only be read from the main thread.
   - The sim receives InputFrames by value and never reads SDL.
   - se_core links no SDL, enforced by the link graph and a banned-include check.
7. On Windows, the modal drag and resize loop blocks the pump.
   - Without main callbacks, SDL sends live-resize EXPOSED events (data1=1) to event watchers.
   - The platform renders from the latest slots inside an SDL_AddEventWatch callback. That callback does not pump; it pushes a heartbeat.
   - The sim keeps 120 Hz throughout, with no restructuring around main callbacks.
8. Vsync and FIFO can block the presenting thread.
   - D3D12: the non-blocking acquire returns NULL instead of blocking (SDL 3.4.18 src/gpu/d3d12/SDL_gpu_d3d12.c:7655). The design adds just-in-time frame starts and its own one-frame fence.
   - Vulkan: SDL's acquire passes an infinite timeout (SDL 3.4.18 src/gpu/vulkan/SDL_gpu_vulkan.c:10242-10245), and FIFO present can block (SDL issues 11293 and 16548).
   - Either way only the platform thread stalls. The sim keeps cadence, and the input-liveness policy handles stale input.
   - Linux has no vblank phase source, so it free-runs. MAILBOX is used where available.
9. The window thread must keep pumping.
   - Every platform wait is bounded and pumps. Shutdown uses a polling loop with a watchdog, and nothing joins inside an event handler.
   - This fixes today's join inside the Closed handler (core/Engine.cpp:66-75).
10. EnTT requirements are met:
   - one owner thread per registry;
   - storages pre-created;
   - ENTT_USE_ATOMIC defined;
   - no parallel writes across owning groups;
   - snapshots copy fields explicitly, because registries are not copyable;
   - matching by full id plus spawn serial.
11. Timers and QoS.
   - The sim waits with absolute high-resolution waits plus a short spin, and never toggles timeBeginPeriod. Today SFML calls timeBeginPeriod every frame (dep/SFML-3.0.2/src/SFML/System/Win32/SleepImpl.cpp:47-54).
   - The platform, sim and worker threads are tagged HighQoS with SetThreadInformation(ThreadPowerThrottling, ControlMask=EXECUTION_SPEED, StateMask=0), and placed on P-cores with CPU Sets. asset-io and log are tagged EcoQoS.
   - Pacing may degrade while the window is minimized or occluded. That is acceptable, because the sim is suspended or paused then.
   - Linux uses clock_nanosleep with TIMER_ABSTIME and lowered timer slack, and no real-time priority.
   - One engine clock: steady_clock, which is QPC on MSVC and CLOCK_MONOTONIC on libstdc++. SDL timestamps are converted through a periodically re-sampled offset.
12. miniaudio has one owner thread, and the device is never started, stopped or uninitialized from its callback.
13. spdlog is configured before any thread starts, runs async with overrun_oldest, and spdlog::shutdown() is called before main returns.
14. Box2D, if added, is stepped on one thread and fed by the shared pool.
15. DXGI's pump-thread deadlock cannot arise, because only the window thread presents.
16. C++ library support: jthread and stop_token, atomic wait and notify, and the enkiTS primitives are all available on MSVC 14.51 and GCC 12 or later. atomic<shared_ptr> is avoided because it is not lock-free.
17. Machine variety is handled by:
   - sizing workers from the CPU topology, not from hardware_concurrency();
   - using P-cores only for the HIGH and MED lanes;
   - running inline below 3 physical cores;
   - on battery, turning donation off and shortening the spin tail (the margin m then grows).

## API sketch

```cpp
// ===== se_core/threading/Role.hpp ===========================================
namespace se {
enum class Role : std::uint8_t { Platform, Sim, Worker, AssetIo, Log };
struct SE_CAPABILITY("role") RoleCap {};
extern RoleCap kPlatformRole, kSimRole;                      // Clang thread-safety role tokens
class SE_SCOPED_CAPABILITY RoleScope {                         // taken once at thread entry
public:
  RoleScope(RoleCap& cap, Role r) SE_ACQUIRE(cap);            // also: thread_local role, Tracy name, QoS, CPU set
  ~RoleScope() SE_RELEASE();
};
Role currentRole() noexcept;
#define SE_ASSERT_ROLE(r) SE_ASSERT(::se::currentRole() == (r))

// ===== se_core/threading/Spsc.hpp ===========================================
inline constexpr std::size_t kCacheLine = 64;                 // engine constant (not the std interference size)
template <class T, std::uint32_t N>                           // N power of two; T trivially copyable or move-only
class SpscRing {
public:
  bool tryPush(T&& v) noexcept;                               // producer; false when full -> caller coalesces/defers/drops
  T*   front() noexcept;                                      // consumer; nullptr when empty (peek)
  void pop() noexcept;                                        // consumer
private:
  alignas(kCacheLine) std::atomic<std::uint32_t> head_{0};    // producer store(release), consumer load(acquire)
  alignas(kCacheLine) std::uint32_t cachedTail_{0};
  alignas(kCacheLine) std::atomic<std::uint32_t> tail_{0};    // consumer store(release), producer load(acquire)
  alignas(kCacheLine) std::uint32_t cachedHead_{0};
  std::array<ManualStorage<T>, N> slots_;
};
class Doorbell {                                              // waits on a monotonic sequence, never a toggling flag
public:
  void ring() noexcept { seq_.fetch_add(1, std::memory_order_release); seq_.notify_one(); }
  std::uint32_t seen() const noexcept { return seq_.load(std::memory_order_acquire); }
  void waitChange(std::uint32_t seen, std::stop_token st) const;   // std::stop_callback rings it
private:
  std::atomic<std::uint32_t> seq_{0};
};

// ===== se_core/sim/Input.hpp ================================================
enum class InputKind : std::uint8_t { Press, Release, Axis, ReleaseAll, Heartbeat };
struct InputEvent { std::uint64_t stampNs, deviceNs; ActionId action; InputKind kind; float value; };
struct InputFrame {
  Tick tick; ActionBits held;
  std::array<std::uint8_t, kMaxActions> pressed, released;     // edges since previous tick: taps are never lost
  std::array<float, kMaxAxes> axes; bool platformLive;
};
class InputFolder {                                            // sim-owned
public:
  InputFrame latch(Tick k, std::uint64_t latchNs, SpscRing<InputEvent, 4096>& ring) SE_REQUIRES(kSimRole);
};

// ===== se_core/sim/Commands.hpp =============================================
struct StartPlay { SessionId session; std::vector<std::byte> worldBeve; std::uint64_t seed; TickRate rate; bool editorAttached; };
struct Step { std::uint32_t ticks; };
struct SetComponentCmd { Guid entity; entt::id_type type; std::vector<std::byte> beve; bool teleport; };
using SimOp = std::variant<StartGame, StartPlay, StopPlay, Pause, Resume, Step, SetTimeScale, SetWakePhase,
                           SetComponentCmd, AddComponentCmd, RemoveComponentCmd, CreateEntityCmd, DestroyEntityCmd,
                           SpawnStress, SelectForMirror, SetDebugDraw, ScopeResident, ViewportChanged, Suspend, Quit>;
struct SimCommand { std::uint32_t seq; std::uint64_t stampNs; SimOp op; };

// ===== se_core/sim/Snapshot.hpp =============================================
struct RenderItem {                                            // ~56 B; keys and ids only, never pointers
  std::uint32_t entity, spawnSerial; Vec2 pos; float rot; Vec2 scale;
  AssetKey texture; std::uint16_t region, animFrame; std::uint8_t layer, flags; float z; Rgba8 tint;
};
static_assert(std::is_trivially_copyable_v<RenderItem>);
struct SnapshotHeader { SessionId session; std::uint32_t generation; Tick tick; std::uint64_t anchorNs, latchNs;
                        std::uint32_t ackCmdSeq, droppedPublishes; float tickCpuMs; };
struct ReliableDeltas { std::vector<TileEdit> tiles; std::vector<AudioEvent> audio; std::vector<ScopeRelease> releases;
                        std::vector<HierarchyDelta> hierarchy; std::vector<SessionEvent> events; };
struct Snapshot {
  SnapshotHeader h; std::vector<RenderItem> items; std::vector<std::uint32_t> sparse;
  std::array<CameraState, kMaxCameras> cams; UiList ui; DebugDrawList debug; EditorPayload editor; ReliableDeltas reliable;
};
class SlotLease;                                               // RAII: const access; destructor returns the slot
class SnapshotChannel {                                        // owns the slot pool; outlives both threads
public:
  explicit SnapshotChannel(std::uint32_t slots = 8);
  Snapshot* acquireForWrite() SE_REQUIRES(kSimRole);           // reclaims returned slots; nullptr => skip render publish
  void publish(Snapshot* s) SE_REQUIRES(kSimRole);             // index push on 'published' (release)
  void receive(std::invocable<SlotLease> auto&& onSlot) SE_REQUIRES(kPlatformRole); // every slot, tick order
private:
  void release(std::uint32_t index) SE_REQUIRES(kPlatformRole);// index push on 'returned' (release)
  std::vector<Snapshot> slots_;
  SpscRing<std::uint32_t, 16> published_, returned_;
  std::vector<std::uint32_t> simFree_;                         // sim-private free list
};
struct FrameView { const Snapshot* a; const Snapshot* b; float alpha; bool cut; Tick shownTick; }; // one per frame

struct Channels {                                              // owned by App
  SpscRing<InputEvent, 4096> input;                            // platform -> sim
  SpscRing<SimCommand, 256> commands; Doorbell simBell;        // platform -> sim
  SnapshotChannel snapshots{8};                                // sim -> platform (+ returns)
  AssetChannels assets;                                        // sim/platform <-> asset-io
  WakeEvent platformWake;                                      // Win32 auto-reset event / SDL user event on Linux
  std::array<FaultBox, 8> faults;
};

// ===== se_core/ecs/World.hpp ================================================
class World {
public:
  World(const ComponentRegistry&, std::uint64_t seed);        // pre-creates every registered storage; owner = caller
  entt::registry& reg();                                      // debug: asserts owner thread
  const entt::registry& creg() const noexcept;                // fork-join readers while the owner does not mutate
  entt::entity create(Guid);                                  // assigns spawn serial
  void requestDestroy(entt::entity);                          // drained once per tick in Cleanup
  SimClock clock; entt::dispatcher events;
private:
  entt::registry reg_; std::vector<std::uint32_t> spawnSerial_; std::thread::id owner_;
};

// ===== se_core/threading/JobPool.hpp ========================================
enum class Lane : std::uint8_t { Render = 0, Sim = 1, Background = 2 };   // enkiTS HIGH / MED / LOW
class JobPool {
public:
  explicit JobPool(const CpuTopology&);                       // workers = perf cores - 2; numExternalTaskThreads = 1
  void registerSimThread();                                   // enki::TaskScheduler::RegisterExternalTaskThread()
  template <class F>                                          // f(chunkIndex, begin, end); chunk is a phase constant
  void parallelFor(Lane, std::uint32_t items, std::uint32_t chunk, F&& f); // caller runs chunks; rethrows first exception
  JobHandle async(Lane, std::move_only_function<void()>);
  void wait(JobHandle, Lane lowestToRun);                     // WaitforTask(task, priority)
  bool donate(Lane lowestToRun, std::uint64_t untilNs);       // WaitforTask(nullptr, prio) until deadline or no work
};

// ===== se_runtime/SimDriver.hpp =============================================
struct SimConfig { TickRate rate{120}; std::uint32_t maxCatchUp{8}; std::chrono::milliseconds dilationAfter{250};
                   std::chrono::microseconds spinTail{300}; bool donate{true}; };
class SimDriver {
public:
  SimDriver(const GameModule&, Channels&, JobPool&, SimConfig);
  void runThreaded(std::stop_token st);                       // sim thread entry
  void tickInline(std::uint64_t frameStartNs) SE_REQUIRES(kPlatformRole);  // inline mode: due ticks on caller
private:
  void tick(Tick k, std::uint64_t latchNs, bool publishRender) SE_REQUIRES(kSimRole);
  //   commands -> input_.latch -> Schedule::run(Input, Intent, Movement, Physics, Effects, GameRules, Cleanup)
  //   -> hash -> extract into acquireForWrite() slot (or carry reliable deltas) -> publish
  void waitUntil(std::uint64_t deadlineNs, std::stop_token st);   // hi-res timer, spin tail, optional donation
  Sessions sessions_; InputFolder input_; ReplayRecorder replay_; PacedClock clock_;  // T0, phi, re-base, dilation
};
void SimDriver::runThreaded(std::stop_token st) {
  RoleScope role(kSimRole, Role::Sim);
  jobs_.registerSimThread();
  guardFaults(Role::Sim, [&] {
    while (!st.stop_requested()) {
      drainCommands();                                        // FIFO, applied at tick boundaries only
      if (!clock_.running()) { ch_.simBell.waitChange(seen_, st); continue; }
      waitUntil(clock_.nextLatchNs(), st);
      const TickBurst due = clock_.dueTicks(steadyNowNs(), cfg_.maxCatchUp, cfg_.dilationAfter); // may re-base T0
      for (Tick k : due) tick(k, clock_.latchNs(k), /*publishRender=*/ k + 2 > due.last());
    }
    sessions_.destroyAll();                                   // Worlds die on their owner thread
  });
}

// ===== se_platform/FrameLoop.hpp ============================================
class FrameLoop {
public:
  int run() SE_REQUIRES(kPlatformRole);                       // returns exit code after ordered, pumping shutdown
private:
  void waitForWork(std::uint64_t deadlineNs);                 // MsgWaitForMultipleObjectsEx({hiResTimer, wake}, QS_ALLINPUT, MWMO_INPUTAVAILABLE)
  void pumpAndRoute();                                        // SDL_PollEvent -> ImGui queue -> InputRouter -> InputEvent ring
  void latchFlush();                                          // 0.3 ms before each sim latch: pads + heartbeat
  void liveResizeFrame();                                     // called from SDL_AddEventWatch (EXPOSED, data1=1); never pumps
  void frame();
  PacingModel pacing_;                                        // DWM vblank phase, r/g p95, safety, phi proposals, own fence
};
void FrameLoop::frame() {
  ch_.snapshots.receive([&](SlotLease s) { applyReliable(*s); retain(std::move(s)); });  // keeps newest 3
  const FrameView view = pickPair(retained_, pacing_.renderTimeNs());                     // tau = F - D'
  JobHandle rl = jobs_.async(Lane::Render, [&] { renderList_.build(view, gpu_.mappedVertices()); });
  if (editor_) editor_->frame(view, simCommands_, docCommands_);                          // ImGui on this thread
  jobs_.wait(rl, Lane::Render);
  gpu_.uploadBudgeted();
  gpu_.recordAndSubmit(renderList_, editor_ ? ImGui::GetDrawData() : nullptr);           // non-blocking acquire
  pacing_.onSubmitted(gpu_.lastFrameFence());
}

// ===== se_runtime/App.hpp ===================================================
class App {
public:
  static std::expected<App, Error> create(const AppConfig&, const GameModule&);
  int  run();                     // platform loop + ordered shutdown; destructors then join already-finished threads
  void requestQuit() noexcept;    // any thread: flag + wake event
private:                          // declaration order = construction order (destruction runs in reverse)
  Logging log_; Platform platform_; GpuDevice gpu_; JobPool jobs_; Audio audio_; AssetService assets_;
  Channels channels_; std::unique_ptr<EditorLayer> editor_; SimThread sim_; FrameLoop loop_;
};
} // namespace se
```

## Changes To Cost Free End State

- Runtime and main loop: 'one thread touches world state' becomes 'one owner thread per World'. Runtime and Play Worlds belong to a dedicated sim thread, and edit documents belong to the platform thread. No World is ever shared. The kept fixed-step accumulator moves into SimDriver as an absolute-deadline scheduler: 120 Hz, at most 8 catch-up ticks, and explicit, reported dilation past 250 ms. That replaces the 0.25 s frame clamp and the 8-step cap, and nothing busy-spins. The platform loop drains events, routes input, builds RenderLists from snapshots, runs the editor UI and presents.
- RenderList builder: its input changes from 'const World + alpha' to a RenderSource. That is a FrameView{S_a, S_b, alpha} at runtime and in Play, or the const document World in Edit. It stays headless and testable, and a headless consumer stands in for the GPU in TSan and determinism tests.
- Editor:
  - Panels take an InspectView (the document World in Edit, the snapshot mirror in Play) instead of always a const World.
  - Play-as-clone through the serializer becomes a move of BEVE bytes to the sim.
  - Pause, Step and Stop become SimCommands, and so do Play-time inspector and gizmo edits.
  - Stop's identical-hash guarantee now holds by structure.
  - Picking moves from the broadphase to the frame's RenderList.
- Input: InputService splits in two. InputRouter runs on the platform: it maps SDL events to actions, gates on ImGui and viewport focus, handles focus loss, sends heartbeats and polls gamepads at each latch. InputFolder runs on the sim and folds events stamped up to each latch into InputFrame k. Also added: an input-liveness policy, and optional raw keyboard input in runtime builds.
- Audio: AudioEmitter output and audio events travel in the snapshot's reliable section. The platform owns ma_engine and schedules each sound at its tick anchor with ma_sound_set_start_time_in_pcm_frames. miniaudio resource-manager jobs run on asset-io.
- Assets: AssetRegistry splits into three parts:
  - an immutable AssetIndex of stable 64-bit AssetKeys, shared read-only;
  - sim-side CPU scopes;
  - platform-side GPU residency.
  Scope release is ordered by a tick watermark plus the frame fence. Snapshots carry keys, not pointers.
- se_core gains a threading module: Role and RoleScope, SpscRing, Doorbell, SnapshotChannel and a JobPool wrapper over enkiTS. World pre-creates all registered storages, asserts its owner thread and assigns spawn serials. ENTT_USE_ATOMIC is defined for every target.
- Systems follow a parallel-safe convention. Parallel phases use const views plus partitioned output spans. Structural changes go only through deferred requests drained in Cleanup. The Schedule marks which phases may fork.
- GameStateStack lives on the sim thread. Loading states wait for both ScopeLoaded and ScopeResident, while the loading screen keeps animating at the tick rate.
- Lifecycle: App::create also builds the pool, the sim jthread, the channels and the topology and QoS policy. App::run ends with an ordered shutdown that keeps pumping events, and destructors only join threads that have already finished. Thread entries and jobs capture exceptions, so a sim fault aborts Play rather than the editor.
- Tests and CI:
  - determinism across inline, threaded, workers 0/1/6 and chaos mode;
  - a TSan job (Linux Clang);
  - clang-cl with -Werror=thread-safety;
  - shutdown and fault-injection tests;
  - se_bench gates on extraction and publish cost, and on platform frame CPU at 144 Hz.
- Profiling: Tracy gets two frame sets (sim and display) and threading plots. The in-editor profiler reads TickStats copied out of slots.
- Platform layer:
  - a Windows-specific wait (MsgWaitForMultipleObjectsEx with a high-resolution waitable timer);
  - DWM vblank phase for just-in-time frames and phase lock;
  - CPU-set placement and QoS tagging;
  - the engine's own one-frame fence limit on top of SDL_SetGPUAllowedFramesInFlight(1).
- Inline mode is a supported configuration, not just a debug switch. The engine selects it on machines with fewer than 3 physical cores. It is also the recommended default if the prototype measures c_p99 low enough for the single loop to hold the display rate (c + r up to about 6.4 ms at 144 Hz).

## Dependencies

### SDL3

**Name:** SDL3

**Choice:** Pin release-3.4.18 (published 2026-10-02 per the GitHub releases API), vendored and built static. 3.4.20 (2026-11-03) is too new to pin.

**Why:** The platform thread owns video, events and the GPU, following SDL's thread rules. Verified in this tag's source:
- the non-blocking D3D12 acquire returns NULL instead of blocking (src/gpu/d3d12/SDL_gpu_d3d12.c:7655);
- D3D12 keeps 2-3 swapchain textures and cycles its fences modulo that count (7022, 8140), so the engine adds its own one-frame fence;
- MAILBOX and IMMEDIATE both present with sync interval 0, and only IMMEDIATE adds the tearing flag (8112-8128);
- SDL_WaitEventTimeout is millisecond-granular (include/SDL3/SDL_events.h:1364), so the engine uses its own Windows wait;
- a raw-input thread can stamp keys (src/video/windows/SDL_windowsrawinput.c:57-116).

### SDL_GPU

**Name:** SDL_GPU

**Choice:** Every call made on the platform thread. Frames in flight set to 1, plus an engine fence. VSYNC by default, with MAILBOX and IMMEDIATE/VRR as options.

**Why:** This sidesteps SDL's undocumented cross-thread resource and ordering rules. SDL_gpu.h has no device-lost API, so recovery means rebuilding the whole device.

### EnTT 3.16.0

**Name:** EnTT 3.16.0

**Choice:** Keep it. Define ENTT_USE_ATOMIC, pre-create storages, and avoid owning groups over pools that are written in parallel.

**Why:** Each registry has one owner thread, and fork-join readers use const registries (dep/entt-3.16.0/docs/md/entity.md:2333-2433).

### Dear ImGui (docking) with first-party imgui_impl_sdl3 and imgui_impl_sdlgpu3

**Name:** Dear ImGui (docking) with first-party imgui_impl_sdl3 and imgui_impl_sdlgpu3

**Choice:** One context on the platform thread. No imgui_club threaded rendering.

**Why:** UI, draw-data preparation and present share one thread, so no draw-data snapshot or texture proxy queue is needed.

### Job system

**Name:** Job system

**Choice:** enkiTS v1.12 (zlib), wrapped by se::JobPool. Fallback: a first-party pool behind the same wrapper.

**Why:** Verified in its header:
- 1-5 priority levels (src/TaskScheduler.h:27-30), used as lanes;
- WaitforTask with a lowest priority to run; with a null task it runs available tasks and returns (347-354), which enables donation;
- external task threads for the sim (284-289, 401);
- profiler callbacks, including threadStart, for names, QoS and CPU sets (253-258).

### SPSC ring

**Name:** SPSC ring

**Choice:** First-party, about 150 lines, with rigtorp-style cached indices. Optional model test.

**Why:** It is the only lock-free code the engine owns, small enough to review and to stress under TSan.

### glaze

**Name:** glaze

**Choice:** BEVE for Play transfer, inspector blobs and replays. JSON for content.

**Why:** One reflection source drives serialize, diff and inspect, and it crosses threads as owned bytes.

### miniaudio

**Name:** miniaudio

**Choice:** 0.11.x, pinned (the fetched header reports 0.11.26). ma_engine lives on the platform thread. The resource manager runs with jobThreadCount = 0, and its jobs run on asset-io.

**Why:** Scheduled starts (ma_sound_set_start_time_in_pcm_frames, miniaudio.h:11469) remove frame-batching jitter, and there is a single game-side owner thread.

### spdlog

**Name:** spdlog

**Choice:** Async logger with 1 worker, 8192 slots and overrun_oldest. An SPSC ring sink feeds the editor.

**Why:** The sim must never block on logging, and spdlog::shutdown() must run before main returns on Windows.

### Tracy

**Name:** Tracy

**Choice:** On-demand mode, named threads, FrameMarkNamed for sim ticks plus FrameMark for display frames, and plots.

**Why:** Makes the two cadences visible, at about 2.25 ns per zone (manual benchmark).

### Catch2 v3, ThreadSanitizer, Clang thread-safety analysis, ASan/UBSan

**Name:** Catch2 v3, ThreadSanitizer, Clang thread-safety analysis, ASan/UBSan

**Choice:** A Linux Clang TSan job; -Werror=thread-safety on clang-cl and Clang; MSVC ASan.

**Why:** Enforces the ownership rules the design depends on.

### Box2D v3

**Name:** Box2D v3

**Choice:** Only if rigid bodies arrive. Pin one version and map its task callbacks onto the sim lane.

**Why:** Keeps a single pool and Box2D's thread-count-independent determinism.

### Windows APIs

**Name:** Windows APIs

**Choice:** dwmapi DwmGetCompositionTimingInfo, CPU Sets, SetThreadInformation (QoS), CreateWaitableTimerExW (high resolution), MsgWaitForMultipleObjectsEx. Optionally AvSetMmThreadCharacteristics.

**Why:** Vblank phase for just-in-time frames and phase lock, P-core placement, and precise waits without timeBeginPeriod.

## Why Not The Others

### Item 1

**Alternative:** Single paced main-thread loop with fork-join inside systems (the end state's current threading position)

**Reason:** A strong option, which is why this design keeps it as inline mode. But it misses the owner's goals as stated: sim and render share one core and one frame budget, so heavy physics does block rendering. Computed: it holds 144 Hz only while c + r stays below about 6.4 ms (c up to 4.4 ms in the game, 2.4 ms in the editor). The sim also stops or bursts whenever the window thread blocks:
- the Windows modal drag loop, unless the loop is restructured around SDL main callbacks;
- vsync or FIFO waits, including Vulkan's acquire with an infinite timeout (SDL issues 11293 and 16548);
- imgui_impl_sdlgpu3's SDL_WaitForGPUIdle when its buffers grow;
- swapchain recreation, message boxes and file dialogs.
Where it wins: about 4.5 ms lower mean input-to-photon at 144 Hz with light ticks (computed: 15.1 vs 19.6 ms), no snapshot cost, and a single call stack. Fork-join inside systems narrows the throughput gap, but not the cadence coupling.

### Item 2

**Alternative:** Job/task-graph runtime: every system and render stage is a task on N workers, possibly pipelining frame N+1's sim with frame N's render

**Reason:** It can reach the same utilization, but ownership becomes a set of per-task dependency declarations instead of per-thread ownership. EnTT's single owner, ImGui's single thread and SDL's main-thread rules all have to be expressed as pinned tasks and resource edges. One missing edge is a silent race that no thread-affinity assert can catch. Independent rates need either a graph per tick plus another per frame, which is this design built from tasks, or an accumulator inside one frame graph, which re-couples the two rates. Debugging is the hardest of the three families: stacks are task frames on arbitrary threads, Tracy shows interleaved tasks, and a schedule-dependent bug needs a deterministic scheduler to reproduce. At 5k colliders and 10k sprites, utilization beyond fork-join inside a tick buys nothing a solo owner can measure.

### Item 3

**Alternative:** In-family variant: a separate render thread records offscreen passes while the platform thread composites and presents

**Reason:** SDL_GPU pins the swapchain, the presenting command buffer and the ImGui frame to the window thread. A render thread could only pre-record offscreen passes. That needs cross-thread GPU ordering, which SDL guarantees only at fence level, and adds a pipeline stage: one more handoff margin of latency, about 1 ms or more. The CPU it would save, about 1 ms on the platform, is already removed by render-lane jobs.

### Item 4

**Alternative:** A shared World behind locks (master's m_mutex, or render-optimization's shared_mutex)

**Reason:** It serializes physics and render, so it gains no concurrency. render-optimization holds the exclusive lock for the whole tick and the shared lock for the whole draw plus ImGui (render-optimization:core/Engine.cpp:215-255, 288-311). Races remain wherever a lock is missing (core/Engine.cpp:204-208). Snapshot handoff gives real overlap with no lock at all.

## When This Loses

- Light ticks at high refresh rates. If the prototype measures c_p99 at or below about 3 ms with platform CPU at or below 2 ms, the single loop holds 144 Hz and is about 4.5 ms faster input-to-photon (computed mean: 15.1 vs 19.6 ms), with no snapshot machinery. Run inline mode there.
- Low core counts or contended machines (2 physical cores, or heavy background load). The two dedicated threads compete with the workers and the OS, so wake jitter m grows and D' grows with it. The engine switches to inline mode below 3 physical cores.
- Laptops on battery. Two threads waking on unrelated cadences (120 Hz ticks, display-rate frames, 120 Hz latch flushes) keep more cores out of deep idle than one loop does. The power cost has not been measured.
- Editor workflows that need direct, synchronous access to the live play World: raycasts or queries on every mouse move, live tables over thousands of entities, or step-and-inspect of a system's internal state. Through mirrors and command round trips these are slower to build and to run.
- Features that need same-frame coupling between sim and render: gameplay that depends on rendered results, or UI that must show the exact current-tick state under the cursor.
- Very cheap simulations, where snapshot extraction (about 0.1 ms) is a large share of the tick and pipelining overlaps almost nothing.
- Display rates where phase lock is impossible (144 Hz and other non-multiples of the tick rate) and neither VRR nor MAILBOX is available. The latency penalty is then the full dt/2 + m.

## Risks

- The case for this design rests on numbers nobody has measured: tick cost c at 5k colliders, snapshot extraction cost, wake jitter m and frame CPU r. If the prototype shows c is small, the design adds machinery for little gain.
- Phase lock and just-in-time frames depend on DWM vblank timing. That timing is assumed accurate for the primary monitor on Windows 11; mixed-refresh multi-monitor setups are unverified. Linux has no source of vblank phase, so it free-runs, at about 4 ms higher mean latency at 60 Hz.
- SDL's D3D12 swapchain keeps at least 2 in-flight fence slots regardless of SDL_SetGPUAllowedFramesInFlight(1) (verified in source). If the engine's own fence limit and pacing go wrong, one queued frame (16.7 ms at 60 Hz) of latency comes back silently.
- SDL_GPU has no device-lost API. Detection through failing calls, and the rebuild path, are untested assumptions.
- Input liveness: a stalled platform leaves the running sim with stale held input. The heartbeat policy (3 ticks, then 100 ms) needs tuning, and its forced releases change gameplay. They are recorded, so runs stay deterministic.
- The Edit and Play inspector backends can diverge, and bugs that appear only through the Play mirror are likely.
- Bugs in the lock-free ring or the slot protocol. TSan runs only on Linux, so Windows-only paths are covered by asserts alone.
- Windows QoS and Thread Director may still move threads to E-cores on hybrid laptops when the window is unfocused. HighQoS tagging is documented as performance-oriented, not as a guarantee of P-core placement.
- Interpolation artifacts: unflagged teleports below the automatic threshold, and discrete state switching at the half-tick.
- Maintenance load for a solo owner. Roles, slots, payloads, pacing models, two drivers and the annotation discipline are more concepts than one loop. The design decays unless the CI gates are kept: TSan, -Wthread-safety, and hash equality across drivers.
- Donation through enkiTS WaitforTask(nullptr) runs whole batches, so a long chunk can delay the sim's latch. Chunks are kept under about 100 us and donation stops 0.5 ms early, but neither has been measured.
- Raw keyboard input on Windows (SDL_HINT_WINDOWS_RAW_KEYBOARD) can interfere with IME and hotkeys, which is why it is runtime-only and off in the editor.
