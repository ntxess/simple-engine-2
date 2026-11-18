# Prototype measurements (Ryzen 7 7700X)

Synthetic measurement rig from the design workflow. Not engine code; numbers inform thresholds that M15 re-measures on the real stack.

## What It Models

This is a measurement rig, not an engine.

World: N entities stored as structure of arrays: x, y, prev x, prev y, vx, vy, half extents 2-6 px, layer 0-3, sprite 0-31, team 0-3 and id. The seeded mt19937 makes runs repeat. Density is 800 px² per entity, so the world side is 2000 px at N=5k and 6325 px at N=50k. Speeds are 20-120 px/s, and input drives entity 0.

Sim step at a fixed 120 Hz, in order:
- Drain the SPSC input queue and record the newest applied sequence number.
- Integrate, bounce off the bounds and bin each entity into a 32 px uniform grid. In mode c this runs in parallel over 2048-entity chunks.
- Counting-sort the entities into cell order (serial).
- Run AABB tests against the 3x3 cell neighbourhood with an "owner computes" half-penetration push: each entity sums only its own push. This runs in parallel over 2-row bands, and contacts are counted once per pair, about 0.29 per entity per tick.
- Apply the pushes (serial).

The spike option busy-spins 25 ms inside ticks 120, 360 and 600 (at 1, 3 and 5 s).

Snapshot: a structure-of-arrays copy of id, x, y, prev x, prev y, layer and sprite, 23 B per entity (112 KB at 5k, 1.1 MB at 50k). A header carries the tick, the applied input sequences, the time origin, the publish time, a checksum and a trailer tick. Snapshots travel through a 3-slot triple buffer: the middle index and a fresh bit share one std::atomic<u32>, and both sides swap with a single exchange(acq_rel). The render side never reads the World, except in the racy control.

Render step:
- Interpolate by alpha. Single-thread uses accumulator/dt; threaded modes use (render start − publish time)/dt clamped to [0,1], which shows the state one tick behind the newest snapshot.
- Cull to a viewport covering the central 50% of the world area. Visible sprites: 2,602 / 5,129 / 10,202 / 25,456 at N=5k/10k/20k/50k.
- std::sort by (layer, sprite, id).
- Fill 4 vertices of 20 B per visible sprite and count batches (128).

Vsync is simulated: a frame is presented at the first vblank of an ideal 59.9 Hz or 144 Hz grid at or after its work ends, and the thread waits for that vblank with a high-resolution waitable timer.

Input: the main thread pushes one timestamped event per 1 ms into a lock-free single-producer single-consumer ring.

Modes:
- a: one loop with a fixed-step accumulator and an 8-step catch-up cap, then render straight from the World.
- a+snapshot: the same loop, but every tick goes through extraction and the triple buffer.
- b: a sim thread and a render thread, with the main thread generating input and controlling the run.
- c: b plus a fork-join pool of W workers; the sim thread also takes chunks.
- control-racy: b, but the render reads the live World. This is a data race on purpose.

Diagnostic variants:
- A re-implementation of the render-optimization branch's TripleBufferedRenderCommands2D (render-optimization:core/util/RenderCommands2D.hpp:59-91).
- An injected reader stall inside acquire.
- The branch's lock pattern: sim unique_lock per tick and render shared_lock per frame (render-optimization:core/Engine.cpp:215,288).
- A late-latch frame start.
- The catch-up cap.

Shutdown uses std::jthread and std::stop_token. A stop_callback sets a per-thread Win32 event, and every wait is WaitForMultipleObjects on {timer, stop event}.

## Method

Each run is one process, for example `mtproto.exe --mode st|st-snap|mt|jobs|racy --n N --disp-hz 59.9|144 --spike 0|1 --workers W`. Runs lasted 5 s (the two triple-buffer stress runs lasted 8 s). They ran one after another from Git Bash under `timeout 60`, plus an internal watchdog that calls std::quick_exit at twice the planned duration. Total measured runtime was about 8 minutes.

Mode codes in runs[]:
- a: single-thread.
- a+snapshot: single-thread through the snapshot path.
- b: two-thread.
- c: two-thread plus jobs, with W pool workers plus the sim thread as caller.
- control-racy: render reads the live World.

Bracket tags:
- branch TB: the branch's triple buffer.
- stall X+0..J us: a reader spin, placed between the branch's load and store, or before the rig's exchange.
- precheck: also verify right after acquire.
- latch X us: the frame starts X after the vblank.
- cap K: at most K catch-up steps per frame.
- lock: the shared_mutex pattern.
- repeat: a rerun of an earlier configuration.

Display: 59.9 Hz (16.694 ms) stands in for 60 Hz. It makes the sim/display phase sweep one full tick per 5 s run. At exactly 60.000 Hz the two-thread latency would depend on one arbitrary fixed phase.

Definitions:
- tick: wall time of one step, excluding extraction.
- catch-up tick, mode a: a tick scheduled at or before one display period before the vblank its frame started on.
- catch-up tick, modes b and c: a tick that started more than dt/2 after its scheduled time.
- hitch: an interval between consecutive present wakes longer than 1.5x the target.
- input->present: from the input event's QPC timestamp to the vblank of the first frame whose snapshot's newest applied input sequence includes it.
- ->render-start: from the same timestamp to the start of that frame's render step.
- past-interp: to the first frame whose older interpolation endpoint already includes the input.
- busy: instrumented work time divided by wall time, from the run's t0 to thread exit. CPU% from QueryThreadCycleTime agreed within about 1 point, after a measured QueryThreadCycleTime/rdtsc ratio of 0.977-0.981.
- publish/acquire: rdtsc around the single exchange, converted at the calibrated 4.4915 TSC ticks/ns. The values include the 9.8 ns rdtsc-pair overhead.
- consistency: after every frame the render recomputes the snapshot checksum and checks header tick == trailer tick == tick at acquire, with ticks monotonic. With precheck it also checks right after acquire.
- racy control: frames whose read overlapped a tick, detected with a seqlock-style counter that is odd during a tick.
- timer lateness: wake time minus deadline. Timer: CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION), relative due time, WaitForMultipleObjects.

Evidence files:
- Self-test (TSC calibration, rdtsc overhead, QueryThreadCycleTime units, timer jitter, Sleep(1), a determinism hash across 0/2/4/6 workers at N=10k and 50k, fork-join cost with 50 us chunks): <scratchpad>/mt-proto\selftest.txt
- Raw RESULT lines, 40-run main matrix: <scratchpad>/mt-proto\results.txt
- 15 extras: <scratchpad>/mt-proto\results_extras.txt
- 16 follow-ups: <scratchpad>/mt-proto\results_final.txt
- 4 latch repeats: <scratchpad>/mt-proto\results_last.txt
- Run scripts: <scratchpad>/mt-proto\run_matrix.sh, <scratchpad>/mt-proto\run_extras.sh and <scratchpad>/mt-proto\run_final.sh
- runs[] was generated from the RESULT files by <scratchpad>/mt-proto\emit_runs4.py, and <scratchpad>/mt-proto\tabulate.py prints the full per-run tables (sim, frame, snap, lat, busy, timer, stop, lock).

All values in runs[] are measured. Values marked "estimated" in the findings are computed from the measured per-entity costs.

## Findings

- [cpu-utilization][bottleneck-idle] At the owner's targets every thread is mostly idle, in either design. Busy time (work/wall) with a 120 Hz sim and 59.9 Hz render: the single loop used 5.9% of one core at N=5,000 and 23.0% at N=20,000 (10,202 visible sprites, the 10k-sprite target). In two-thread mode at N=20,000 the sim thread was 21.6% busy and the render thread 3.5%, so the render thread was idle about 96% of the time. At N=50,000 the sim thread becomes the bottleneck (53.9%) while the render thread stays at 8.9%. Six job workers cut the sim thread to 16.7%, with 6.5% per worker (results.txt). The single loop left 77-94% of each frame free at the target loads, so the second core adds headroom the targets do not use.
- [scalability] Sim cost grew linearly: tick p50 0.40 / 0.79 / 1.63 / 4.20 ms at N=5k/10k/20k/50k. In two-thread mode it never touched render time: render work p50 was the same across modes at each N (0.118-0.121, 0.248-0.258, 0.517-0.532 and 1.389-1.420 ms). In single-thread the frame's critical path is 2 ticks plus the render: 1.0 / 1.8 / 3.8 / 9.8 ms at those N, which is 6 / 11 / 23 / 59% of a 16.69 ms frame. Estimated by linear extrapolation of the measured per-entity costs (0.084 us per entity per tick, 0.028 us per entity per frame): the single loop runs out of 59.9 Hz budget near N=85k, and out of 144 Hz budget near N=35k.
- [frame-latency] In single-thread a 25 ms physics spike always reached the frame cadence. In two-thread it never did, but it reached the screen in both. Single-thread showed 2 hitches per run at N=5k-20k (frame max 33.3-33.9 ms, one missed vblank per spike). The 20 two-thread and jobs spike runs, plus the 144 Hz one, had 0 hitches caused by the spikes: frame max ≤ 17.6 ms (7.5 ms at 144 Hz). The 3 jobs runs that had one hitch each had it at an OS stall away from the spikes (2132, 2515 and 4196 ms). World motion still froze and then jumped in both designs: two-thread runs had 2 motion stalls and 2 jumps per run with alpha clamped in 5-10 frames, and single-thread runs had 1-2 stalls and 2 jumps. The second thread keeps the frame cadence (UI, camera and editor stay responsive), not the smoothness of the simulated world.
- [frame-latency] Single-thread has one measured failure mode at N=50,000. After the first spike the loop stayed at 30 Hz for the rest of the run: 115 hitches, frame p50 33.2 ms, 232 catch-up ticks, reproduced in a repeat (113 hitches, 229 catch-ups). The cause: after one missed vblank every frame owes 4 ticks × 4.2 ms + 1.4 ms of render = 18.2 ms, which is more than 16.69 ms, so it misses again. Two-thread mode at N=50k had 0 hitches and absorbed 10 catch-up ticks on the sim thread. This comes from the catch-up policy, not from the lack of a thread: the same single loop with a 3-step catch-up cap had 2 hitches and 6 catch-up ticks (results_final.txt).
- [independent-rates] Separate rates start to matter once 2 ticks plus the render exceed one display period. At 144 Hz and N=10,000 both modes had 0 hitches and showed 720/720 frames. At 144 Hz and N=50,000 single-thread had 17 hitches (703/720 frames, frame p99 13.9 ms), because frames that run 2 ticks need 2×4.2+1.4 = 9.8 ms against a 6.94 ms period. Two-thread had 0 hitches and 720/720 frames (results_extras.txt). The 120 Hz sim ran 600/600 ticks in every mt and jobs run at both display rates.
- [sync-overhead] The lock-free snapshot boundary is cheap:
  - Extraction (memcpy of 23 B per entity), p50: 6.4 us at 5k (112 KB), 12 us at 10k (225 KB), 24 us at 20k (449 KB), 59-66 us at 50k (1.1 MB). That is 17-19 GB/s, at most 1.6% of the tick and 0.8% of the 8.33 ms tick budget.
  - Publish exchange: p50 10-20 ns, at the 9.8 ns rdtsc-pair floor; p99 at most 190 ns.
  - Acquire: p50 30-200 ns across cores against 10 ns on the same core (a+snapshot); p99 at most 641 ns.
  - Reading the snapshot from another core did not slow the render pass. Interp+cull p50 at 10k was 70.2 us (a), 70.2 us (a+snapshot) and 71.2 us (b); at 50k it was 351.6 / 348.1 / 347.6 us, on one CCD with 32 MB of shared L3.
  - The rig's checksum and verify passes cost about 2× the copy. They are test machinery, not design cost.
- [sync-overhead][frame-latency] Any lock on the render path brings the spike back. I reproduced the branch's pattern: the sim takes unique_lock per tick and the render takes shared_lock per frame (render-optimization:core/Engine.cpp:215 and :288). At N=10k with the spike, the render waited up to 26.8 ms for the lock and showed 2 hitches (frame max 33.6 ms), the same as single-thread. At N=50k with no spike, the render lock wait was p50 0.51 ms, p99 4.65 ms and max 4.89 ms, and the sim waited up to 1.8 ms.
- [sync-overhead][scalability] Fork-join jobs speed up the sim but carry a fixed cost. In the self-test (selftest.txt), 50 us chunks spread over W+1 participants finished in 55-66 us p50 back-to-back, and in 67-80 us p50 when the workers had to be woken from sleep once per 8.33 ms (ideal 50 us; p99 92-260 us, max 398 us). In the runs, tick p50 at N=50k fell from 4.20 ms to 1.86 / 1.36 / 1.18 ms with 2 / 4 / 6 workers (3.6×), and at N=5k from 0.398 to 0.134 ms (3.0×). The narrowphase scaled 6.3× (3426 to 542 us). The serial counting sort (415-446 us) then sets the limit, at about 35% of the 6-worker tick. The parallel integrate+bin pass was slower than serial at N=5k (18.9 against 11.3 us), because the fork-join cost was larger than the work.
- [inconsistent-snapshots][races] Snapshots fixed consistency; reading the live World did not. With the rig's single-exchange triple buffer there were 0 violations in 17,512 verified frames over 54 runs, 705 of which were also checked before rendering. The control that renders straight from the live World is today's pattern (render at core/Engine.cpp:208 reading what update at :181-183 writes). It read during a tick in progress in 36 of 299 frames (12%) at N=10k and in 203 of 299 (68%) at N=50k (results_extras.txt).
- [races] The triple buffer on the unmerged render-optimization branch (TripleBufferedRenderCommands2D, render-optimization:core/util/RenderCommands2D.hpp:62-83) has a race:
  - The reader loads m_readyIndex at line 80 and only stores m_readIndex at line 81. A writer that publishes twice in that gap can reuse the buffer the reader just chose.
  - I re-implemented it over the rig's snapshot, stalled the reader at that point for a random 9-16 ms (standing in for preemption) and checked the checksum before rendering. Result: 3 torn frames out of 406 at N=50k.
  - The rig's buffer under the same stall had 0 of 406.
  - With no stall, or with only an after-render check, both showed 0 (299 frames each). The race fires rarely, and a test can easily miss it.
- [frame-latency] At equal pacing a second thread adds input latency. At 59.9 Hz and N=10k, input-to-present p50/p99 was 24.0/32.2 ms single-thread and 29.6/40.5 ms two-thread (+5.6/+8.3 ms). Measured until the input has fully passed interpolation, it was 24.6 against 37.9 ms (+13.3 ms), because the threaded interpolation shows the state one tick behind the newest snapshot. In single-thread the tick that consumes the input runs right before the render. In two-thread the input first waits for the sim's own tick phase and then for the render's next frame. N=50k: 20.4/28.7 against 33.3/44.2 ms (jobs6 30.1/41.0). At 144 Hz and N=10k: 10.7/20.0 against 14.9/21.5 ms.
- [frame-latency] Late latching wins back most of that latency in both designs; under a heavy sim only the threaded design can afford it. Starting each frame 12 ms after the vblank at N=10k gave 11.9/20.2 ms single-thread and 17.6/28.6 ms two-thread, both with 0 hitches; the gap between them stayed about 5.7 ms. At N=50k the single loop could not afford it: a 4 ms latch put it into 30 Hz (77 hitches), and a 2 ms latch with a 3-step cap gave 18.9/43.0 ms with 8 hitches. With a 13 ms latch, two-thread gave 20.5/32.5 ms (2 hitches) and jobs6 gave 17.4/35.0 and 17.2/28.6 ms (3 and 1 hitches). An earlier two-thread 13 ms run is excluded: outside interference pushed its tick p99 to 12.9 ms and it had 8 hitches.
- [lifecycle-shutdown] Cooperative shutdown with jthread and stop_token was bounded by the longest piece of work that cannot be interrupted. Stopping 10 ms into a 25 ms spike took 14.1-15.4 ms in all 25 runs, the time for the sim thread to finish its tick. The render join was ≤ 0.24 ms and the pool join 0.05-0.30 ms. There was no hang in 75 runs. Two things made this work:
  - Every wait also watches a stop event.
  - The parallel-for caller claims chunks itself, so it can always finish without the workers.
- [debugging] Threading did not cost determinism here, because the algorithm was built for it. The owner-computes narrowphase gave bitwise-identical world hashes after 240 ticks with 0, 2, 4 and 6 workers, at both N=10k and N=50k (selftest.txt). A bug in jobs mode can therefore be narrowed down by rerunning with 0 workers. Building the rig itself showed the debugging cost:
  - My first pool design let a stale worker pair an old chunk index with a new job's total. Review caught it, and I fixed it by packing both into one atomic.
  - My first consistency check ran only after rendering, so it could not see a torn copy that finished during the reader's stall. I found that only after a 0 result looked too clean.
  The single-thread mode needs none of this machinery.
- [debugging][bottleneck-idle] There are rare stalls I could not explain. In 5 of 29 job-pool runs, the sim and render threads both woke 12-29 ms late from their timer waits at the same instant (for example jobs4 at N=10k with the spike: 23.3 ms and 22.6 ms late at about 4.19 s). There were 0 such events in 28 mt/racy runs and 0 in 3 repeats of jobs4 at N=10k. If the rate were equal, all 5 landing in pool runs would have about a 3% chance, so the pool link is suggestive. But the stall also hits threads that never touch the pool. This is open: capture an ETW or Tracy trace before trusting tail latency in jobs mode on Windows.
- [timer] The high-resolution waitable timer (CreateWaitableTimerExW with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION) was late by 195-313 us p50 and 519-665 us p99 in the self-tests, with a maximum of 995 us. During runs it was typically late by 204-318 us p50 and 490-940 us p99. For comparison, Sleep(1) took 1.50 ms p50. Spinning for the last 200 us cut the p50 to 71-77 us, but p99 stayed at 335-423 us. This jitter puts every frame-interval p99 at 17.0-17.5 ms for a 16.69 ms target, but it never caused a hitch by itself.
- [overall] For the stated targets on this machine, the numbers do not show the second thread paying for itself. A paced single loop with a catch-up cap that fits the frame budget had 0 hitches without spikes at 59.9 Hz up to N=50k, and at 144 Hz up to N=10k. It used 5.9-23% of one core at the target loads and had 4-6 ms lower input-latency p50 (8-13 ms lower once interpolation is complete). The measured advantages of two threads appear only beyond the targets or under stress: N=50k at 144 Hz, a spike longer than a frame (the cadence holds but the motion still freezes), and late latching under a heavy sim. Done lock-free, its costs are small (snapshot ≤ 66 us per tick, exchange ≤ 20 ns p50). They show up as input latency, test and verification machinery, and races that fire rarely.

## Caveats

- This is not an engine. There is no GPU, driver or compositor. Vsync is simulated on an ideal 59.9 Hz or 144 Hz grid, and 'present' means that grid time. Real present and scanout latency was not measured; it would add the same amount to every mode.
- 59.9 Hz stands in for 60 Hz so that the sim/display phase sweeps one full tick per 5 s run. At exactly 60.000 Hz the two-thread latency would depend on one fixed, arbitrary phase. I estimate that phase would move p50 by up to ±4 ms.
- All runs were on one machine: a Ryzen 7 7700X with a single CCD and 32 MB of shared L3, on Windows 11 in a desktop session with other apps running and an unknown power plan; the process opted out of power throttling. Cross-core costs would be higher on multi-CCD parts and on Intel hybrid CPUs, where a thread can land on an E-core. Laptops on battery and Linux were not tested.
- Each configuration ran once for 5 s (about 600 ticks and 300 frames), except the runs marked as repeats. A p99 therefore rests on about 3-6 samples, and a max on a single event. Repeats of two-thread at N=10k varied by 0.15 ms in latency p50 and 0.2 ms in p99. Late in the session the frame-interval p50 drifted from 16.69-16.73 ms to 16.51-16.65 ms, which means the timer behaviour changed during the session (cause unknown).
- The workload is synthetic: uniform random AABBs at 800 px² per entity, about 0.29 contacts per entity per tick. The spike is a 25 ms busy spin, not memory-bound physics. Absolute costs scale with density and with real game logic; only the relationships between modes carry over.
- Input is consumed at the start of every tick, as the task specified. A real single-thread engine that polls OS events once per frame would apply slightly fewer late inputs in a frame's second tick. The input generator wakes every 1 ms and is about 0.3 ms late.
- Jobs mode parallelises only integrate+bin and the narrowphase; the counting sort, the push apply and extraction stay serial. Workers sleep on WaitOnAddress with no spin, so wake-up costs are included. A spinning pool would cut that overhead and use more CPU.
- The racy control is a deliberate data race, which is undefined behaviour in C++. It counts frames whose read overlapped a tick, not how bad they look.
- The branch triple-buffer test re-implements the branch's algorithm over the rig's snapshot type; it does not compile the branch's code. The 9-16 ms reader stall is artificial, and in production the window would need preemption between two adjacent instructions.
- CPU% from QueryThreadCycleTime assumes the cycles count at TSC rate; the measured ratio against rdtsc was 0.977-0.981. Publish and acquire times include the 9.8 ns rdtsc-pair overhead, so an uncontended exchange is below the measurement floor.
- The late-latch runs at N=50k are sensitive to background load. One of them was disturbed and is reported but excluded from the conclusions. The 5 OS stalls of 12-29 ms in job-pool runs remain unexplained.
- Determinism was checked only for this MSVC /O2 binary across worker counts. Behaviour across compilers and platforms was not tested.

## Shutdown

Stop protocol: the main thread calls request_stop on the sim (or loop) jthread and the render jthread, joins the sim, joins the render, then stops the pool with request_stop, notify and join. Every timed wait is WaitForMultipleObjects on {high-resolution waitable timer, per-thread stop event}, and a std::stop_callback sets that event. Pool workers sleep in std::atomic::wait, and their stop_callback bumps the generation and calls notify_all. The parallel-for caller claims chunks itself, so a stopped pool can never strand it.

Measured: in all 25 runs where the stop was requested 10 ms into a 25 ms spike, the total join took 14.1-15.4 ms, which is the sim thread finishing its uninterruptible tick. The render join was at most 0.24 ms (at most 0.005 ms without the lock), and the pool join took 0.05-0.30 ms. Stops outside a spike took 0.08-10.9 ms, finishing the current tick.

There was no hang in 75 runs. The watchdog (std::quick_exit at twice the planned duration) never fired, every run exited 0, and tasklist showed no mtproto.exe left after each batch.

Implication: shutdown latency equals the longest unit of work that cannot be interrupted, here one tick. The same bound applies to scene changes or pauses requested from another thread, in both designs.

## Runs

### a N=5000 display=59.9 workers=0 spike=none

**Name:** a N=5000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.398/0.699/7.496; 599/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.699/17.239/18.693; 0; 299/299 [ms; count]
- render work p50/p99 = 0.118/0.326 [ms]
- input->present p50/p99; ->render-start p50; past-interp p50 = 24.43/32.66; 8.91; 24.62 [ms]
- busy work/wall = loop 5.9 [%]


### b N=5000 display=59.9 workers=0 spike=none

**Name:** b N=5000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.401/0.604/0.777; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.710/17.109/17.129; 0; 299/299 [ms; count]
- render work p50/p99 = 0.119/0.234 [ms]
- extract p50/p99; size; publish p50/p99; acquire p50/p99 = 6.4/11.7 us; 112.4 KB; 10/30 ns; 120/471 ns [mixed]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.06/39.96; 12.70; 37.37 [ms]
- busy work/wall = sim 5.2, render 0.8 [%]


### c N=5000 display=59.9 workers=2 spike=none

**Name:** c N=5000 display=59.9 workers=2 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.189/0.323/0.484; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.726/17.207/17.288; 0; 299/299 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 28.98/39.86; 12.65; 37.31 [ms]
- busy work/wall = sim 2.6, render 0.8, worker avg 1.5 [%]


### c N=5000 display=59.9 workers=4 spike=none

**Name:** c N=5000 display=59.9 workers=4 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.146/0.277/8.978; 600/600/3 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.708/17.182/39.979; 1; 298/299 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.06/40.05; 12.70; 37.41 [ms]
- busy work/wall = sim 2.4, render 0.8, worker avg 0.9 [%]
- max timer wake lateness sim|render = 6582.0|23510.7 [us]


### c N=5000 display=59.9 workers=6 spike=none

**Name:** c N=5000 display=59.9 workers=6 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.134/0.238/0.372; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.706/17.165/17.383; 0; 299/299 [ms; count]
- render work p50/p99 = 0.121/0.178 [ms]
- extract p50/p99; size; publish p50/p99; acquire p50/p99 = 6.4/11.2 us; 112.4 KB; 10/20 ns; 190/571 ns [mixed]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.01/39.95; 12.68; 37.32 [ms]
- busy work/wall = sim 1.9, render 0.8, worker avg 0.7 [%]


### a N=10000 display=59.9 workers=0 spike=none

**Name:** a N=10000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.791/1.078/1.197; 599/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.708/17.094/17.134; 0; 299/299 [ms; count]
- render work p50/p99 = 0.258/0.366 [ms]
- input->present p50/p99; ->render-start p50; past-interp p50 = 24.00/32.15; 9.22; 24.63 [ms]
- busy work/wall = loop 11.2 [%]


### b N=10000 display=59.9 workers=0 spike=none

**Name:** b N=10000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.799/1.165/1.367; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.721/17.135/17.268; 0; 299/299 [ms; count]
- render work p50/p99 = 0.248/0.483 [ms]
- extract p50/p99; size; publish p50/p99; acquire p50/p99 = 12.0/20.4 us; 224.7 KB; 10/20 ns; 60/381 ns [mixed]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.60/40.45; 13.20; 37.93 [ms]
- busy work/wall = sim 10.3, render 1.6 [%]


### c N=10000 display=59.9 workers=2 spike=none

**Name:** c N=10000 display=59.9 workers=2 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.360/0.483/0.555; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.710/17.166/17.239; 0; 299/299 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.05/40.03; 12.70; 37.42 [ms]
- busy work/wall = sim 4.9, render 1.6, worker avg 2.9 [%]


### c N=10000 display=59.9 workers=4 spike=none

**Name:** c N=10000 display=59.9 workers=4 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.271/0.376/0.514; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.706/17.142/17.206; 0; 299/299 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 28.93/39.76; 12.52; 37.29 [ms]
- busy work/wall = sim 3.8, render 1.6, worker avg 1.8 [%]


### c N=10000 display=59.9 workers=6 spike=none

**Name:** c N=10000 display=59.9 workers=6 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.251/0.364/0.554; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.695/17.418/17.540; 0; 299/299 [ms; count]
- render work p50/p99 = 0.251/0.317 [ms]
- extract p50/p99; size; publish p50/p99; acquire p50/p99 = 12.5/20.5 us; 224.7 KB; 10/30 ns; 140/481 ns [mixed]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.03/39.88; 12.67; 37.34 [ms]
- busy work/wall = sim 3.5, render 1.6, worker avg 1.3 [%]


### a N=20000 display=59.9 workers=0 spike=none

**Name:** a N=20000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 1.625/2.125/2.259; 599/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.691/17.207/17.354; 0; 299/299 [ms; count]
- render work p50/p99 = 0.532/0.723 [ms]
- input->present p50/p99; ->render-start p50; past-interp p50 = 23.05/31.25; 9.95; 24.65 [ms]
- busy work/wall = loop 23.0 [%]


### b N=20000 display=59.9 workers=0 spike=none

**Name:** b N=20000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 1.635/2.640/8.238; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.706/17.303/21.394; 0; 299/299 [ms; count]
- render work p50/p99 = 0.530/1.837 [ms]
- extract p50/p99; size; publish p50/p99; acquire p50/p99 = 25.5/53.6 us; 449.3 KB; 20/110 ns; 50/361 ns [mixed]
- input->present p50/p99; ->render-start p50; past-interp p50 = 30.67/41.53; 14.27; 38.97 [ms]
- busy work/wall = sim 21.6, render 3.5 [%]
- max timer wake lateness sim|render = 1029.2|5523.3 [us]


### c N=20000 display=59.9 workers=2 spike=none

**Name:** c N=20000 display=59.9 workers=2 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.707/1.087/4.394; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.687/17.166/17.209; 0; 299/299 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.62/40.51; 13.23; 37.88 [ms]
- busy work/wall = sim 9.8, render 3.3, worker avg 5.8 [%]


### c N=20000 display=59.9 workers=4 spike=none

**Name:** c N=20000 display=59.9 workers=4 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.534/1.406/29.196; 600/600/4 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.706/17.154/22.679; 0; 299/299 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.51/40.33; 13.11; 37.86 [ms]
- busy work/wall = sim 8.4, render 3.5, worker avg 3.6 [%]
- max timer wake lateness sim|render = 5273.8|12538.5 [us]


### c N=20000 display=59.9 workers=6 spike=none

**Name:** c N=20000 display=59.9 workers=6 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.469/0.680/0.778; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.687/17.216/17.336; 0; 299/299 [ms; count]
- render work p50/p99 = 0.523/0.757 [ms]
- extract p50/p99; size; publish p50/p99; acquire p50/p99 = 24.1/46.9 us; 449.3 KB; 20/70 ns; 120/471 ns [mixed]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.44/40.28; 13.01; 37.78 [ms]
- busy work/wall = sim 6.7, render 3.3, worker avg 2.6 [%]


### a N=50000 display=59.9 workers=0 spike=none

**Name:** a N=50000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 4.202/5.031/5.386; 599/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.696/17.151/17.586; 0; 299/299 [ms; count]
- render work p50/p99 = 1.409/1.677 [ms]
- input->present p50/p99; ->render-start p50; past-interp p50 = 20.39/28.68; 12.47; 24.55 [ms]
- busy work/wall = loop 59.3 [%]


### b N=50000 display=59.9 workers=0 spike=none

**Name:** b N=50000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 4.236/5.045/5.714; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.695/17.178/17.349; 0; 299/299 [ms; count]
- render work p50/p99 = 1.420/2.093 [ms]
- extract p50/p99; size; publish p50/p99; acquire p50/p99 = 66.4/112.2 us; 1123.1 KB; 20/100 ns; 110/441 ns [mixed]
- input->present p50/p99; ->render-start p50; past-interp p50 = 33.31/44.16; 16.93; 41.60 [ms]
- busy work/wall = sim 53.9, render 8.9 [%]


### c N=50000 display=59.9 workers=2 spike=none

**Name:** c N=50000 display=59.9 workers=2 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 1.865/2.402/2.772; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.710/17.188/17.550; 0; 299/299 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 30.87/41.83; 14.45; 39.18 [ms]
- busy work/wall = sim 25.0, render 8.7, worker avg 14.2 [%]


### c N=50000 display=59.9 workers=4 spike=none

**Name:** c N=50000 display=59.9 workers=4 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 1.361/1.845/2.314; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.695/17.222/17.384; 0; 299/299 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 30.19/41.09; 13.80; 38.56 [ms]
- busy work/wall = sim 18.9, render 8.6, worker avg 8.8 [%]


### c N=50000 display=59.9 workers=6 spike=none

**Name:** c N=50000 display=59.9 workers=6 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 1.180/1.624/2.155; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.711/17.181/17.402; 0; 299/299 [ms; count]
- render work p50/p99 = 1.389/1.757 [ms]
- extract p50/p99; size; publish p50/p99; acquire p50/p99 = 59.2/97.0 us; 1123.1 KB; 20/40 ns; 50/331 ns [mixed]
- input->present p50/p99; ->render-start p50; past-interp p50 = 30.10/40.97; 13.75; 38.40 [ms]
- busy work/wall = sim 16.7, render 8.6, worker avg 6.5 [%]


### a N=5000 display=59.9 workers=0 spike=25ms/2s

**Name:** a N=5000 display=59.9 workers=0 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.393/0.657/25.473; 600/602/4 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.703/17.315/33.619; 2; 298/300 [ms; count]
- render work p50/p99 = 0.118/0.194 [ms]
- input->present p50/p99; ->render-start p50; past-interp p50 = 24.59/36.93; 9.01; 24.79 [ms]
- busy work/wall = loop 7.0 [%]
- alpha-clamped/stall/jump frames = 0/2/2 [count]
- join after mid-spike stop = 14.806 [ms]


### b N=5000 display=59.9 workers=0 spike=25ms/2s

**Name:** b N=5000 display=59.9 workers=0 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.402/0.675/25.488; 600/601/4 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.720/17.140/17.149; 0; 300/300 [ms; count]
- render work p50/p99 = 0.120/0.209 [ms]
- extract p50/p99; size; publish p50/p99; acquire p50/p99 = 6.8/11.7 us; 112.4 KB; 10/20 ns; 140/491 ns [mixed]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.27/40.87; 12.86; 37.50 [ms]
- busy work/wall = sim 6.7, render 0.8 [%]
- alpha-clamped/stall/jump frames = 6/2/2 [count]
- join after mid-spike stop = 14.665 [ms]


### c N=5000 display=59.9 workers=2 spike=25ms/2s

**Name:** c N=5000 display=59.9 workers=2 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.190/0.361/25.309; 600/601/4 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.687/17.212/17.367; 0; 300/300 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.15/40.91; 12.75; 37.48 [ms]
- busy work/wall = sim 4.1, render 0.8, worker avg 1.5 [%]
- alpha-clamped/stall/jump frames = 10/2/2 [count]
- join after mid-spike stop = 14.202 [ms]


### c N=5000 display=59.9 workers=4 spike=25ms/2s

**Name:** c N=5000 display=59.9 workers=4 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.153/1.007/25.163; 600/601/8 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.728/17.422/42.531; 1; 299/300 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.14/41.24; 12.76; 37.51 [ms]
- busy work/wall = sim 4.0, render 0.8, worker avg 0.9 [%]
- alpha-clamped/stall/jump frames = 9/2/2 [count]
- join after mid-spike stop = 14.369 [ms]
- max timer wake lateness sim|render = 17202.9|27100.7 [us]


### c N=5000 display=59.9 workers=6 spike=25ms/2s

**Name:** c N=5000 display=59.9 workers=6 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.137/0.258/25.243; 600/601/4 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.699/17.157/17.489; 0; 300/300 [ms; count]
- render work p50/p99 = 0.121/0.179 [ms]
- extract p50/p99; size; publish p50/p99; acquire p50/p99 = 6.7/12.1 us; 112.4 KB; 10/30 ns; 130/411 ns [mixed]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.18/41.04; 12.79; 37.49 [ms]
- busy work/wall = sim 3.4, render 0.8, worker avg 0.7 [%]
- alpha-clamped/stall/jump frames = 10/2/2 [count]
- join after mid-spike stop = 14.371 [ms]


### a N=10000 display=59.9 workers=0 spike=25ms/2s

**Name:** a N=10000 display=59.9 workers=0 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.784/1.733/25.871; 600/602/4 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.698/17.166/33.865; 2; 298/300 [ms; count]
- render work p50/p99 = 0.256/0.648 [ms]
- input->present p50/p99; ->render-start p50; past-interp p50 = 24.07/36.69; 9.30; 24.75 [ms]
- busy work/wall = loop 12.9 [%]
- alpha-clamped/stall/jump frames = 0/1/2 [count]
- join after mid-spike stop = 14.121 [ms]


### b N=10000 display=59.9 workers=0 spike=25ms/2s

**Name:** b N=10000 display=59.9 workers=0 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.795/1.189/25.833; 600/601/4 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.682/17.192/17.291; 0; 300/300 [ms; count]
- render work p50/p99 = 0.248/0.390 [ms]
- extract p50/p99; size; publish p50/p99; acquire p50/p99 = 12.5/24.8 us; 224.7 KB; 10/20 ns; 120/481 ns [mixed]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.66/41.39; 13.27; 38.02 [ms]
- busy work/wall = sim 11.8, render 1.6 [%]
- alpha-clamped/stall/jump frames = 7/2/2 [count]
- join after mid-spike stop = 14.146 [ms]


### c N=10000 display=59.9 workers=2 spike=25ms/2s

**Name:** c N=10000 display=59.9 workers=2 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.376/0.909/39.115; 600/601/8 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.705/17.140/45.426; 1; 299/300 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.15/41.04; 12.75; 37.56 [ms]
- busy work/wall = sim 7.4, render 1.6, worker avg 3.0 [%]
- alpha-clamped/stall/jump frames = 5/3/3 [count]
- join after mid-spike stop = 15.191 [ms]
- max timer wake lateness sim|render = 1528.8|28859.1 [us]


### c N=10000 display=59.9 workers=4 spike=25ms/2s

**Name:** c N=10000 display=59.9 workers=4 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.278/1.278/25.312; 600/601/8 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.704/17.518/34.306; 1; 299/300 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.08/43.33; 12.74; 37.42 [ms]
- busy work/wall = sim 5.5, render 1.8, worker avg 1.8 [%]
- alpha-clamped/stall/jump frames = 8/2/2 [count]
- join after mid-spike stop = 14.472 [ms]
- max timer wake lateness sim|render = 23314.5|22607.0 [us]


### c N=10000 display=59.9 workers=6 spike=25ms/2s

**Name:** c N=10000 display=59.9 workers=6 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.244/0.675/25.286; 600/601/4 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.710/17.143/17.355; 0; 300/300 [ms; count]
- render work p50/p99 = 0.249/0.369 [ms]
- extract p50/p99; size; publish p50/p99; acquire p50/p99 = 12.3/20.8 us; 224.7 KB; 10/30 ns; 150/511 ns [mixed]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.06/40.85; 12.66; 37.38 [ms]
- busy work/wall = sim 5.1, render 1.6, worker avg 1.3 [%]
- alpha-clamped/stall/jump frames = 6/2/2 [count]
- join after mid-spike stop = 15.202 [ms]


### a N=20000 display=59.9 workers=0 spike=25ms/2s

**Name:** a N=20000 display=59.9 workers=0 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 1.600/2.220/26.713; 600/602/4 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.703/17.226/33.338; 2; 298/300 [ms; count]
- render work p50/p99 = 0.525/0.699 [ms]
- input->present p50/p99; ->render-start p50; past-interp p50 = 23.16/35.69; 10.00; 24.71 [ms]
- busy work/wall = loop 24.2 [%]
- alpha-clamped/stall/jump frames = 0/2/2 [count]
- join after mid-spike stop = 14.852 [ms]


### b N=20000 display=59.9 workers=0 spike=25ms/2s

**Name:** b N=20000 display=59.9 workers=0 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 1.600/2.112/26.830; 600/601/6 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.723/17.178/17.323; 0; 300/300 [ms; count]
- render work p50/p99 = 0.518/0.752 [ms]
- extract p50/p99; size; publish p50/p99; acquire p50/p99 = 24.4/42.7 us; 449.3 KB; 20/70 ns; 60/301 ns [mixed]
- input->present p50/p99; ->render-start p50; past-interp p50 = 30.65/42.33; 14.26; 39.00 [ms]
- busy work/wall = sim 22.0, render 3.3 [%]
- alpha-clamped/stall/jump frames = 9/2/2 [count]
- join after mid-spike stop = 15.190 [ms]


### c N=20000 display=59.9 workers=2 spike=25ms/2s

**Name:** c N=20000 display=59.9 workers=2 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.688/0.929/25.796; 600/601/4 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.708/17.128/17.325; 0; 300/300 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.64/41.37; 13.24; 38.00 [ms]
- busy work/wall = sim 10.8, render 3.3, worker avg 5.6 [%]
- alpha-clamped/stall/jump frames = 5/2/2 [count]
- join after mid-spike stop = 14.752 [ms]


### c N=20000 display=59.9 workers=4 spike=25ms/2s

**Name:** c N=20000 display=59.9 workers=4 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.525/0.797/25.645; 600/601/4 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.695/17.167/17.289; 0; 300/300 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.62/41.28; 13.26; 37.94 [ms]
- busy work/wall = sim 8.9, render 3.3, worker avg 3.5 [%]
- alpha-clamped/stall/jump frames = 10/2/2 [count]
- join after mid-spike stop = 15.209 [ms]


### c N=20000 display=59.9 workers=6 spike=25ms/2s

**Name:** c N=20000 display=59.9 workers=6 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.447/0.679/25.493; 600/601/4 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.715/17.189/17.266; 0; 300/300 [ms; count]
- render work p50/p99 = 0.517/0.768 [ms]
- extract p50/p99; size; publish p50/p99; acquire p50/p99 = 23.2/42.5 us; 449.3 KB; 20/100 ns; 120/331 ns [mixed]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.33/40.97; 12.90; 37.66 [ms]
- busy work/wall = sim 7.9, render 3.2, worker avg 2.6 [%]
- alpha-clamped/stall/jump frames = 9/2/2 [count]
- join after mid-spike stop = 15.321 [ms]


### a N=50000 display=59.9 workers=0 spike=25ms/2s

**Name:** a N=50000 display=59.9 workers=0 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 4.030/5.153/29.776; 600/602/232 [ms; count]
- frame p50/p99/max; hitches; frames/due = 33.192/49.941/50.225; 115; 183/300 [ms; count]
- render work p50/p99 = 1.400/1.671 [ms]
- input->present p50/p99; ->render-start p50; past-interp p50 = 32.39/54.54; 17.75; 36.59 [ms]
- busy work/wall = loop 55.3 [%]
- alpha-clamped/stall/jump frames = 0/1/2 [count]
- join after mid-spike stop = 14.413 [ms]


### b N=50000 display=59.9 workers=0 spike=25ms/2s

**Name:** b N=50000 display=59.9 workers=0 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 4.135/5.351/29.579; 600/601/10 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.718/17.199/17.558; 0; 300/300 [ms; count]
- render work p50/p99 = 1.407/1.833 [ms]
- extract p50/p99; size; publish p50/p99; acquire p50/p99 = 62.8/114.2 us; 1123.1 KB; 20/100 ns; 110/301 ns [mixed]
- input->present p50/p99; ->render-start p50; past-interp p50 = 33.31/44.99; 16.88; 41.63 [ms]
- busy work/wall = sim 54.0, render 8.8 [%]
- alpha-clamped/stall/jump frames = 8/2/2 [count]
- join after mid-spike stop = 14.813 [ms]


### c N=50000 display=59.9 workers=2 spike=25ms/2s

**Name:** c N=50000 display=59.9 workers=2 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 1.822/2.325/27.234; 600/601/6 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.729/17.177/17.242; 0; 300/300 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 30.81/42.51; 14.37; 39.11 [ms]
- busy work/wall = sim 25.6, render 8.6, worker avg 14.0 [%]
- alpha-clamped/stall/jump frames = 9/2/2 [count]
- join after mid-spike stop = 15.426 [ms]


### c N=50000 display=59.9 workers=4 spike=25ms/2s

**Name:** c N=50000 display=59.9 workers=4 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 1.349/1.899/27.022; 600/601/6 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.709/17.176/17.255; 0; 300/300 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 30.38/42.13; 13.99; 38.73 [ms]
- busy work/wall = sim 20.2, render 8.6, worker avg 8.7 [%]
- alpha-clamped/stall/jump frames = 8/2/2 [count]
- join after mid-spike stop = 14.838 [ms]


### c N=50000 display=59.9 workers=6 spike=25ms/2s

**Name:** c N=50000 display=59.9 workers=6 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 1.186/1.808/26.689; 600/601/6 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.693/17.201/17.542; 0; 300/300 [ms; count]
- render work p50/p99 = 1.397/1.788 [ms]
- extract p50/p99; size; publish p50/p99; acquire p50/p99 = 59.2/109.5 us; 1123.1 KB; 20/50 ns; 110/411 ns [mixed]
- input->present p50/p99; ->render-start p50; past-interp p50 = 30.35/42.06; 13.94; 38.58 [ms]
- busy work/wall = sim 18.3, render 8.7, worker avg 6.5 [%]
- alpha-clamped/stall/jump frames = 8/2/2 [count]
- join after mid-spike stop = 14.890 [ms]


### a N=10000 display=144 workers=0 spike=none

**Name:** a N=10000 display=144 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.791/1.170/1.259; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 6.998/7.384/7.664; 0; 720/720 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 10.69/20.03; 4.89; 18.66 [ms]
- busy work/wall = loop 13.6 [%]


### b N=10000 display=144 workers=0 spike=none

**Name:** b N=10000 display=144 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.790/1.123/1.469; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 7.031/7.353/7.565; 0; 720/720 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 14.94/21.47; 8.24; 23.26 [ms]
- busy work/wall = sim 10.3, render 3.7 [%]


### a N=10000 display=144 workers=0 spike=25ms/2s

**Name:** a N=10000 display=144 workers=0 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.792/1.134/25.857; 600/601/4 [ms; count]
- frame p50/p99/max; hitches; frames/due = 6.991/7.387/27.588; 2; 714/721 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 10.77/20.67; 4.92; 18.71 [ms]
- busy work/wall = loop 14.9 [%]
- alpha-clamped/stall/jump frames = 0/2/2 [count]


### b N=10000 display=144 workers=0 spike=25ms/2s

**Name:** b N=10000 display=144 workers=0 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.795/1.159/25.888; 600/601/4 [ms; count]
- frame p50/p99/max; hitches; frames/due = 6.978/7.362/7.479; 0; 721/721 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 14.88/21.87; 8.21; 23.19 [ms]
- busy work/wall = sim 11.7, render 3.7 [%]
- alpha-clamped/stall/jump frames = 6/6/3 [count]


### a N=50000 display=144 workers=0 spike=none

**Name:** a N=50000 display=144 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 4.232/5.220/6.800; 600/600/13 [ms; count]
- frame p50/p99/max; hitches; frames/due = 7.039/13.918/14.266; 17; 703/720 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 10.94/21.45; 8.44; 18.89 [ms]
- busy work/wall = loop 71.4 [%]


### b N=50000 display=144 workers=0 spike=none

**Name:** b N=50000 display=144 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 4.290/5.367/8.888; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 6.970/7.382/8.424; 0; 720/720 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 18.80/25.59; 12.17; 27.18 [ms]
- busy work/wall = sim 54.4, render 20.9 [%]


### a+snapshot N=10000 display=59.9 workers=0 spike=none

**Name:** a+snapshot N=10000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.790/1.065/1.407; 599/600/0 [ms; count]
- extract p50/p99; size; publish p50/p99; acquire p50/p99 = 12.6/26.6 us; 224.7 KB; 10/30 ns; 10/70 ns [mixed]
- interp+cull p50 = 70.2 [us]
- input->present p50/p99; ->render-start p50; past-interp p50 = 23.99/32.14; 9.27; 24.61 [ms]
- busy work/wall = loop 11.7 [%]
- consistency violations = 0 of 299 frames [count]


### a+snapshot N=50000 display=59.9 workers=0 spike=none

**Name:** a+snapshot N=50000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 4.107/4.986/5.436; 599/600/0 [ms; count]
- extract p50/p99; size; publish p50/p99; acquire p50/p99 = 61.3/110.1 us; 1123.1 KB; 20/120 ns; 10/50 ns [mixed]
- interp+cull p50 = 348.1 [us]
- input->present p50/p99; ->render-start p50; past-interp p50 = 20.24/28.61; 12.53; 24.51 [ms]
- busy work/wall = loop 60.3 [%]
- consistency violations = 0 of 299 frames [count]


### control-racy N=10000 display=59.9 workers=0 spike=none

**Name:** control-racy N=10000 display=59.9 workers=0 spike=none

**Metrics:**

- frames read during a tick in progress = 36 of 299 [count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.59/40.45; 13.21; 37.85 [ms]
- busy work/wall = sim 9.7, render 1.5 [%]


### control-racy N=50000 display=59.9 workers=0 spike=none

**Name:** control-racy N=50000 display=59.9 workers=0 spike=none

**Metrics:**

- frames read during a tick in progress = 203 of 299 [count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 33.13/44.00; 16.73; 41.42 [ms]
- busy work/wall = sim 50.2, render 8.7 [%]


### b [branch TB] N=10000 display=59.9 workers=0 spike=none

**Name:** b [branch TB] N=10000 display=59.9 workers=0 spike=none

**Metrics:**

- consistency violations = 0 of 299 frames [count]
- fresh frames = 298 [count]
- frame p50/p99/max; hitches; frames/due = 16.721/17.170/17.187; 0; 299/299 [ms; count]


### b [branch TB, stall 10000 us] N=10000 display=59.9 workers=0 spike=none

**Name:** b [branch TB, stall 10000 us] N=10000 display=59.9 workers=0 spike=none

**Metrics:**

- consistency violations = 0 of 299 frames [count]
- fresh frames = 270 [count]
- frame p50/p99/max; hitches; frames/due = 16.723/17.128/17.172; 0; 299/299 [ms; count]


### b [stall 10000 us] N=10000 display=59.9 workers=0 spike=none

**Name:** b [stall 10000 us] N=10000 display=59.9 workers=0 spike=none

**Metrics:**

- consistency violations = 0 of 299 frames [count]
- fresh frames = 299 [count]
- frame p50/p99/max; hitches; frames/due = 16.733/17.151/17.259; 0; 299/299 [ms; count]


### b [lock] N=10000 display=59.9 workers=0 spike=25ms/2s

**Name:** b [lock] N=10000 display=59.9 workers=0 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.788/1.148/25.899; 600/601/4 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.720/17.465/33.630; 2; 298/300 [ms; count]
- render lock wait p50/p99/max; sim lock wait p99/max = 0.000/0.828/26.757; 0.248/0.290 [ms]
- input->present p50/p99; ->render-start p50; past-interp p50 = 28.76/40.11; 12.41; 37.03 [ms]


### b [lock] N=50000 display=59.9 workers=0 spike=none

**Name:** b [lock] N=50000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 4.079/4.893/5.697; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.713/17.280/17.436; 0; 299/299 [ms; count]
- render lock wait p50/p99/max; sim lock wait p99/max = 0.513/4.654/4.890; 1.097/1.808 [ms]
- input->present p50/p99; ->render-start p50; past-interp p50 = 28.63/39.49; 13.39; 36.83 [ms]


### a [latch 12000 us] N=10000 display=59.9 workers=0 spike=none

**Name:** a [latch 12000 us] N=10000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.793/1.137/1.343; 598/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.714/17.170/17.178; 0; 299/299 [ms; count]
- render work p99 = 0.336 [ms]
- input->present p50/p99; ->render-start p50; past-interp p50 = 11.93/20.18; 9.17; 12.74 [ms]


### b [latch 12000 us] N=10000 display=59.9 workers=0 spike=none

**Name:** b [latch 12000 us] N=10000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.810/1.233/1.408; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.702/17.224/17.584; 0; 299/299 [ms; count]
- render work p99 = 0.379 [ms]
- input->present p50/p99; ->render-start p50; past-interp p50 = 17.63/28.56; 13.22; 26.01 [ms]


### a [latch 4000 us] N=50000 display=59.9 workers=0 spike=none

**Name:** a [latch 4000 us] N=50000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 4.151/4.985/5.110; 597/600/79 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.917/33.824/33.921; 77; 221/299 [ms; count]
- render work p99 = 2.054 [ms]
- input->present p50/p99; ->render-start p50; past-interp p50 = 22.35/49.18; 15.33; 26.45 [ms]


### b [latch 13000 us] N=50000 display=59.9 workers=0 spike=none

**Name:** b [latch 13000 us] N=50000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 4.520/12.937/18.289; 600/600/56 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.710/38.919/50.555; 8; 289/299 [ms; count]
- render work p99 = 6.722 [ms]
- input->present p50/p99; ->render-start p50; past-interp p50 = 20.22/53.45; 16.78; 28.52 [ms]


### c [latch 13000 us] N=50000 display=59.9 workers=6 spike=none

**Name:** c [latch 13000 us] N=50000 display=59.9 workers=6 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 1.277/2.619/5.927; 600/600/1 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.704/20.177/33.755; 1; 298/299 [ms; count]
- render work p99 = 1.981 [ms]
- input->present p50/p99; ->render-start p50; past-interp p50 = 17.17/28.62; 13.75; 25.62 [ms]


### b [branch TB, stall 9000+0..7000 us, precheck] N=50000 display=59.9 workers=0 spike=none

**Name:** b [branch TB, stall 9000+0..7000 us, precheck] N=50000 display=59.9 workers=0 spike=none

**Metrics:**

- consistency violations = 3 of 406 frames, 3 caught pre-render [count]
- fresh frames = 298 [count]
- frame p50/p99/max; hitches; frames/due = 16.782/33.696/33.814; 73; 406/479 [ms; count]


### b [stall 9000+0..7000 us, precheck] N=50000 display=59.9 workers=0 spike=none

**Name:** b [stall 9000+0..7000 us, precheck] N=50000 display=59.9 workers=0 spike=none

**Metrics:**

- consistency violations = 0 of 406 frames, 0 caught pre-render [count]
- fresh frames = 405 [count]
- frame p50/p99/max; hitches; frames/due = 16.778/33.739/33.760; 73; 406/479 [ms; count]


### b [precheck] N=50000 display=59.9 workers=0 spike=none

**Name:** b [precheck] N=50000 display=59.9 workers=0 spike=none

**Metrics:**

- consistency violations = 0 of 299 frames, 0 caught pre-render [count]
- fresh frames = 298 [count]
- frame p50/p99/max; hitches; frames/due = 16.710/17.243/17.312; 0; 299/299 [ms; count]


### a [repeat] N=50000 display=59.9 workers=0 spike=25ms/2s

**Name:** a [repeat] N=50000 display=59.9 workers=0 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 4.080/5.001/29.882; 600/602/229 [ms; count]
- frame p50/p99/max; hitches; frames/due = 33.184/49.996/50.200; 113; 185/300 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 31.88/54.72; 17.76; 36.15 [ms]


### a [cap 3] N=50000 display=59.9 workers=0 spike=25ms/2s

**Name:** a [cap 3] N=50000 display=59.9 workers=0 spike=25ms/2s

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 4.166/4.997/29.517; 600/608/6 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.731/17.185/50.302; 2; 299/303 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 20.58/44.95; 12.61; 24.73 [ms]


### c [repeat] N=10000 display=59.9 workers=4 spike=none

**Name:** c [repeat] N=10000 display=59.9 workers=4 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.275/0.403/0.461; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.719/17.125/17.191; 0; 299/299 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.04/39.93; 12.67; 37.42 [ms]
- max timer wake lateness sim|render = 913.7|641.9 [us]


### b [repeat] N=10000 display=59.9 workers=0 spike=none

**Name:** b [repeat] N=10000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.808/1.177/1.296; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.510/17.159/19.038; 0; 299/299 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.55/40.34; 13.01; 37.97 [ms]
- max timer wake lateness sim|render = 2664.8|2391.7 [us]


### c [repeat] N=10000 display=59.9 workers=4 spike=none

**Name:** c [repeat] N=10000 display=59.9 workers=4 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.271/0.435/5.240; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.603/17.148/17.580; 0; 299/299 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 28.99/39.88; 12.51; 37.27 [ms]
- max timer wake lateness sim|render = 2694.8|1035.2 [us]


### b [repeat] N=10000 display=59.9 workers=0 spike=none

**Name:** b [repeat] N=10000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.818/1.145/2.777; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.508/17.040/17.167; 0; 299/299 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.65/40.56; 13.16; 37.98 [ms]
- max timer wake lateness sim|render = 822.4|567.2 [us]


### c [repeat] N=10000 display=59.9 workers=4 spike=none

**Name:** c [repeat] N=10000 display=59.9 workers=4 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.272/0.404/0.547; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.508/17.095/17.119; 0; 299/299 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 28.99/39.83; 12.51; 37.30 [ms]
- max timer wake lateness sim|render = 632.6|547.5 [us]


### b [repeat] N=10000 display=59.9 workers=0 spike=none

**Name:** b [repeat] N=10000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 0.806/1.159/1.227; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.652/17.163/17.220; 0; 299/299 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 29.70/40.55; 13.30; 37.98 [ms]
- max timer wake lateness sim|render = 738.3|833.1 [us]


### b [repeat] N=50000 display=59.9 workers=0 spike=none

**Name:** b [repeat] N=50000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 4.203/5.498/14.747; 600/600/5 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.555/17.197/17.250; 0; 299/299 [ms; count]
- input->present p50/p99; ->render-start p50; past-interp p50 = 33.43/44.62; 17.00; 41.75 [ms]
- max timer wake lateness sim|render = 2298.7|804.7 [us]


### b [latch 13000 us, repeat] N=50000 display=59.9 workers=0 spike=none

**Name:** b [latch 13000 us, repeat] N=50000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 4.267/6.259/9.910; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.523/17.159/34.900; 2; 297/299 [ms; count]
- render work p99 = 1.814 [ms]
- input->present p50/p99; ->render-start p50; past-interp p50 = 20.45/32.49; 17.00; 28.80 [ms]


### c [latch 13000 us, repeat] N=50000 display=59.9 workers=6 spike=none

**Name:** c [latch 13000 us, repeat] N=50000 display=59.9 workers=6 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 1.172/2.942/9.169; 600/600/0 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.551/30.829/36.372; 3; 296/299 [ms; count]
- render work p99 = 2.571 [ms]
- input->present p50/p99; ->render-start p50; past-interp p50 = 17.41/34.97; 14.00; 25.69 [ms]


### a [latch 2000 us, cap 3] N=50000 display=59.9 workers=0 spike=none

**Name:** a [latch 2000 us, cap 3] N=50000 display=59.9 workers=0 spike=none

**Metrics:**

- tick p50/p99/max; run/due/catch-up = 4.183/5.769/16.778; 591/600/13 [ms; count]
- frame p50/p99/max; hitches; frames/due = 16.516/33.505/33.699; 8; 291/299 [ms; count]
- render work p99 = 2.188 [ms]
- input->present p50/p99; ->render-start p50; past-interp p50 = 18.85/43.01; 12.78; 23.12 [ms]

