# Pending verification gates

Gates that need hardware or setup the development machine (Ryzen 7 7700X,
RTX 3080 and AMD iGPU, two monitors, Windows 11) does not have. They never
block a milestone; each lists the exact command to run once the setup exists.

| Gate | Milestone | Needs | Command / procedure | Status |
|---|---|---|---|---|
| WSL Linux toolchain | M0 | One-time install in WSL Ubuntu 24.04 | `sudo apt install -y clang lld ninja-build python3-pip pipx glslang-tools && pipx install cmake` | done 2026-11-08 (clang 18.1.3, CMake 4.4.4, ninja 1.11.1, glslang 15.1) |
| Linux debug build and tests | M0 | WSL toolchain above | `wsl -d Ubuntu -- bash -lc "bash /mnt/d/Dev/simple-engine-2/scripts/wsl-build.sh linux-clang-debug"` | passing (M0) |
| ThreadSanitizer | M0, M4, M14, M17 | WSL toolchain above | `... scripts/wsl-build.sh linux-clang-tsan` | passing (M0); re-run at every milestone |
| ASan + UBSan on Linux | every milestone | WSL toolchain above | `... scripts/wsl-build.sh linux-clang-asan` | passing (M0) |
| Linux determinism goldens (detmath, determinism_kernel) | M2 | WSL toolchain above | first run: `SE_UPDATE_GOLDENS=1 ... scripts/wsl-build.sh linux-clang-debug`, check the new tests/golden/linux-clang/*.txt against tests/golden/windows-msvc (they should match: MSVC and clang-cl agree bit for bit), then commit them | pending |
| SPIR-V shader compilation | M7 | glslang-tools in WSL (installed) | rebuild the `se_shaders` target | ready for M7 |
| Modal drag keeps ticking | M5 | an interactive desktop session | `ctest --preset msvc-dev -L desktop -R modal` | pending |
| Paced presentation under 1% misses | M6 | PresentMon | `tools/presentmon/run.ps1 -Mode Paced` | pending |
| VRR runs 120 Hz Locked | M6 | a VRR display | `tools/presentmon/run.ps1 -Mode Vrr` | pending |
| Displayed-time latency within 1.5 frames | M6 | PresentMon and a photodiode | latency harness, docs/design/mt-design.md tests | pending |
| Gamepad 1 kHz sampling and edge placement | M8 | a gamepad | `ctest -L desktop -R gamepad` | pending |
| Sampling at 250 Hz on battery | M8 | a laptop on battery | as above | pending |
| Sleep/resume and debugger do not trigger the watchdog | M12 | manual sleep and debugger attach | procedure in tests/platform/watchdog_manual.md | pending |
| E-core calibration | M14 | a 2P+8E hybrid CPU | `se_bench --calibrate-ecores` | pending |
| Thresholds on weak machines | M15 | 4-core laptop, 2P+8E laptop on battery | `se_bench --suite real-stack` | pending |
| Linux GPU stacks | M15, M18 | Mesa/Wayland and NVIDIA/X11 machines | `se_bench --suite real-stack`, rampage smoke | pending |
| 10-minute fork-site soak | M16 | the weakest supported machine | `se_bench --soak <site> --minutes 10` | pending |
