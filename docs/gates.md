# Pending verification gates

Gates that need hardware or setup the development machine (Ryzen 7 7700X,
RTX 3080 and AMD iGPU, two monitors, Windows 11) does not have. They never
block a milestone; each lists the exact command to run once the setup exists.

| Gate | Milestone | Needs | Command / procedure | Status |
|---|---|---|---|---|
| WSL Linux toolchain | M0 | One-time install in WSL Ubuntu 24.04 | `sudo apt install -y clang-20 lld-20 libclang-rt-20-dev ninja-build python3-pip pipx glslang-tools && pipx install cmake` (clang 18 cannot use libstdc++'s `std::expected`) | done 2026-11-08 (clang 20.1.2, CMake 4.4.4, ninja 1.11.1, glslang 15.1) |
| Linux debug build and tests | M0 | WSL toolchain above | `wsl -d Ubuntu -- bash -lc "bash /mnt/d/Dev/simple-engine-2/scripts/wsl-build.sh linux-clang-debug"` | passing (M0) |
| ThreadSanitizer | M0, M4, M14, M17 | WSL toolchain above | `... scripts/wsl-build.sh linux-clang-tsan` | passing (M0); re-run at every milestone |
| ASan + UBSan on Linux | every milestone | WSL toolchain above | `... scripts/wsl-build.sh linux-clang-asan` | passing (M0) |
| Linux determinism goldens (detmath, determinism_kernel, determinism_rampage) | M2, M3 | WSL toolchain above | `SE_UPDATE_GOLDENS=1 bash scripts/wsl-build.sh linux-clang-debug`, then compare tests/golden/linux-clang/*.txt with tests/golden/windows-msvc and commit them | done 2026-11-08 (M2; determinism_rampage in M3): bit-identical to windows-msvc and windows-clangcl apart from the header line naming the platform |
| Legacy live recording replays headless (`legacy.selfdet.live_replay`) | M1 | an interactive desktop session (a visible window for about 5 s); the legacy test presets exclude label `desktop` | `scripts/ci.ps1 -Presets legacy-msvc-debug`, then `ctest --test-dir out/build/legacy-msvc-debug -L desktop` | passed by hand 2026-11-08; rerun after changes to core/Engine.cpp or core/util/LegacyInput.* |
| SPIR-V shader compilation | M7 | glslang-tools in WSL (installed) | rebuild the `se_shaders` target | ready for M7 |
| Modal drag keeps ticking | M5 | a person (or SendInput) dragging the title bar: a programmatic move loop needs the mouse button down (`WM_SYSCOMMAND SC_MOVE\|HTCAPTION` is ignored without it) or moves the cursor (keyboard `SC_MOVE`), which desktop tests may not do | run `out/build/msvc-dev/bin/rampage.exe`, hold the title bar for a few seconds while moving it; the title's tick keeps advancing and the log shows the ticks continuing (about 120 per second). The automated `platform.Ticks continue during a modal loop with Modal set` (`scripts/ci.ps1 -Desktop`) runs a menu modal loop instead: the same Modal flag and the same WM_TIMER-driven iterates | menu-loop variant passing 2026-11-08 (1.0 s modal, 123 ticks, at most 2 per frame); real drag pending |
| Paced presentation under 1% misses | M6 | PresentMon | `tools/presentmon/run.ps1 -Mode Paced` | pending |
| VRR runs 120 Hz Locked | M6 | a VRR display | `tools/presentmon/run.ps1 -Mode Vrr` | pending |
| Displayed-time latency within 1.5 frames | M6 | PresentMon and a photodiode | latency harness, docs/design/mt-design.md tests | pending |
| Gamepad 1 kHz sampling and edge placement | M8 | a gamepad | `ctest -L desktop -R gamepad` | pending |
| Sampling at 250 Hz on battery | M8 | a laptop on battery | as above | pending |
| Sleep/resume and debugger do not trigger the watchdog | M12 | manual sleep and debugger attach | procedure in tests/platform/watchdog_manual.md | pending |
| E-core calibration | M14 | a 2P+8E hybrid CPU | `se_bench --calibrate-ecores` | pending |
| Release bench gates on an idle machine (`bench.renderlist_10k`, `bench.rampage_tick_5k`) | M3 | no other builds or benchmarks running: other processes widen p95 (renderlist_10k measured 1.2-2.5 ms p95 under parallel builds against 0.45-0.59 ms idle) | `scripts/ci.ps1 -Presets msvc-release`; results in out/build/msvc-release/bench_out/*.json | passing 2026-11-08 at the M3 integration (renderlist_10k p95 0.45 ms of 1.0; rampage_tick_5k p95 2.88 ms of 6.22); rerun idle at every milestone |
| Thresholds on weak machines | M15 | 4-core laptop, 2P+8E laptop on battery | `se_bench --suite real-stack` | pending |
| Linux GPU stacks | M15, M18 | Mesa/Wayland and NVIDIA/X11 machines | `se_bench --suite real-stack`, rampage smoke | pending |
| 10-minute fork-site soak | M16 | the weakest supported machine | `se_bench --soak <site> --minutes 10` | pending |
