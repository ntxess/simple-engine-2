# Platform and library threading constraints

Researched 2026-11-08 from SDL3, Dear ImGui, EnTT, miniaudio, Tracy, spdlog and Windows documentation and sources.

## Constraints

### Video init and windows

**Area:** Video init and windows

**Library:** SDL3 (3.4.x)

**Rule:** SDL_Init (including SDL_INIT_VIDEO), SDL_CreateWindow and window queries such as SDL_GetWindowSizeInPixels 'should only be called on the main thread'. On Apple the main thread is the thread that runs main(); on other platforms it is 'the one that calls SDL_Init(SDL_INIT_VIDEO)'.

**Source:** https://wiki.libsdl.org/SDL3/SDL_Init ; https://wiki.libsdl.org/SDL3/SDL_CreateWindow ; https://wiki.libsdl.org/SDL3/SDL_GetWindowSizeInPixels ; https://wiki.libsdl.org/SDL3/SDL_IsMainThread

**Confidence:** documented

**Implication:** One platform thread owns every window and every window query for the life of the process. On Windows and Linux it can be any thread that calls SDL_Init(SDL_INIT_VIDEO), not necessarily main(); this does not port to macOS. Other threads get window size and DPI from a per-frame packet or ask through SDL_RunOnMainThread.

### Event pump

**Area:** Event pump

**Library:** SDL3

**Rule:** SDL_PumpEvents and SDL_PollEvent run on the main thread only. SDL_PollEvent 'may implicitly call SDL_PumpEvents(), you can only call this function in the thread that initialized the video subsystem'. On Windows it can block 'for significant amounts of time' while a window is dragged or resized. SDL_PushEvent and SDL_RunOnMainThread are safe from any thread. RunOnMainThread callbacks are queued and run 'during event processing'.

**Source:** https://wiki.libsdl.org/SDL3/SDL_PumpEvents ; https://wiki.libsdl.org/SDL3/SDL_PollEvent ; https://wiki.libsdl.org/SDL3/SDL_PushEvent ; https://wiki.libsdl.org/SDL3/SDL_RunOnMainThread

**Confidence:** documented

**Implication:** All OS input arrives on the platform thread. Workers send requests to it with SDL_PushEvent or SDL_RunOnMainThread. A worker must never call SDL_RunOnMainThread with wait_complete=true while the platform thread may be waiting on that worker, because that deadlocks. Queued callbacks are delayed while the platform thread is blocked in vsync.

### Win32 message queue

**Area:** Win32 message queue

**Library:** Windows (User32)

**Rule:** The system posts input 'to the message queue of the thread that created the destination window'. For PeekMessage, 'The window must belong to the current thread', and hWnd=NULL retrieves only the current thread's windows. If a top-level window stops responding to messages for more than several seconds, Windows replaces it with a ghost window.

**Source:** https://learn.microsoft.com/en-us/windows/win32/winmsg/about-messages-and-message-queues ; https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-peekmessagew

**Confidence:** documented

**Implication:** The window thread must keep pumping through long work, shutdown joins and asset loads. Any blocking wait on that thread (a join, a vsync acquire, a fence wait) adds directly to input latency.

### Window drag and resize on Windows

**Area:** Window drag and resize on Windows

**Library:** SDL3

**Rule:** On WM_ENTERSIZEMOVE, SDL calls SetTimer(USER_TIMER_MINIMUM) and resets the keyboard ('we won't get any key up events during the modal loop'). Each WM_TIMER calls SDL_OnWindowLiveResizeUpdate. That function runs SDL_IterateMainCallbacks when the app uses main callbacks; otherwise it sends SDL_EVENT_WINDOW_EXPOSED with data1=1, which 'can be redrawn directly from event watchers'. Event watchers 'may run in a different thread'.

**Source:** https://github.com/libsdl-org/SDL/blob/main/src/video/windows/SDL_windowsevents.c ; https://github.com/libsdl-org/SDL/blob/main/src/video/SDL_video.c (SDL_OnWindowLiveResizeUpdate) ; https://wiki.libsdl.org/SDL3/SDL_EventType ; https://wiki.libsdl.org/SDL3/SDL_AddEventWatch

**Confidence:** verified-in-source

**Implication:** A plain single-thread loop (poll, step, draw) stops stepping the simulation for the whole drag. Keeping simulation time running needs either SDL main callbacks, so the modal-loop timer drives iteration, or a sim thread that does not depend on the event pump.

### GPU command buffers

**Area:** GPU command buffers

**Library:** SDL3 SDL_GPU

**Rule:** The SDL3 documentation states:
- 'The command buffer may only be used on the thread it was acquired on' and 'should be submitted on the thread it was acquired on'.
- Several command buffers may be open on one thread.
- 'Complex scenes can use multiple command buffers, maybe configured across multiple threads in parallel, as long as they are submitted in the correct order'.
- A submit guarantees only that commands 'begin executing before any command in a subsequent submission begins executing'.
In the source, the Vulkan backend keys its command pools by SDL_ThreadID, and both backends serialize acquire and submit with internal mutexes (acquireCommandBufferLock, submitLock).

**Source:** https://wiki.libsdl.org/SDL3/SDL_AcquireGPUCommandBuffer ; https://wiki.libsdl.org/SDL3/CategoryGPU ; https://github.com/libsdl-org/SDL/blob/main/include/SDL3/SDL_gpu.h ; src/gpu/vulkan/SDL_gpu_vulkan.c ; src/gpu/d3d12/SDL_gpu_d3d12.c

**Confidence:** documented

**Implication:** Recording in parallel is supported, one command buffer per thread, and a buffer is never handed from one thread to another. The engine owns submission order across threads. For example, the recording thread signals 'frame N submitted' before the platform thread submits the composite that samples frame N.

### Swapchain and present

**Area:** Swapchain and present

**Library:** SDL3 SDL_GPU

**Rule:** SDL_ClaimWindowForGPUDevice, SDL_AcquireGPUSwapchainTexture, SDL_WaitAndAcquireGPUSwapchainTexture and SDL_WaitForGPUSwapchain 'should only be called from the thread that created the window' ('You MUST NOT call this function from any thread other than the one that created the window'). The swapchain texture 'should only be referenced by the command buffer used to acquire it', and it is presented when that command buffer is submitted.

**Source:** https://wiki.libsdl.org/SDL3/SDL_ClaimWindowForGPUDevice ; https://wiki.libsdl.org/SDL3/SDL_AcquireGPUSwapchainTexture ; https://wiki.libsdl.org/SDL3/SDL_WaitAndAcquireGPUSwapchainTexture ; https://wiki.libsdl.org/SDL3/SDL_WaitForGPUSwapchain

**Confidence:** documented

**Implication:** Windows can only be created on the SDL main thread, so the presenting command buffer must be acquired, recorded and submitted on that thread. A separate render thread can only render offscreen (scene targets, editor viewports) into its own command buffers. The final blit or composite, ImGui and present stay on the platform thread. This is the main platform limit on any render-thread design.

### GPU validation

**Area:** GPU validation

**Library:** SDL3 SDL_GPU

**Rule:** src/gpu/SDL_gpu.c contains no thread-ownership check for command buffers or swapchain calls. Debug validation covers only pass state and submission state, for example 'Cannot acquire a swapchain texture during a pass!'.

**Source:** https://github.com/libsdl-org/SDL/blob/main/src/gpu/SDL_gpu.c

**Confidence:** verified-in-source

**Implication:** Using these objects from the wrong thread is silent undefined behaviour. The engine has to assert thread identity itself with SDL_GetCurrentThreadID or SDL_IsMainThread, both of which are safe to call from any thread.

### VSYNC and frame pacing

**Area:** VSYNC and frame pacing

**Library:** SDL3 SDL_GPU

**Rule:** Present modes and acquire behaviour:
- Claimed swapchains start as SDR + VSYNC. VSYNC is always supported; IMMEDIATE and MAILBOX 'may not be supported on certain systems'.
- VSYNC enqueues behind a pending image; MAILBOX replaces the pending image.
- Acquire: 'In VSYNC present mode (which is the default) this function may block on vblank'.
- The Wait variant 'Blocks the thread until a swapchain texture is available'.
- A NULL texture means too many frames are in flight or the window is minimized; it is not an error.

**Source:** https://wiki.libsdl.org/SDL3/SDL_GPUPresentMode ; https://wiki.libsdl.org/SDL3/SDL_SetGPUSwapchainParameters ; https://wiki.libsdl.org/SDL3/SDL_AcquireGPUSwapchainTexture ; https://wiki.libsdl.org/SDL3/SDL_WaitAndAcquireGPUSwapchainTexture

**Confidence:** documented

**Implication:** The platform thread can block for up to one refresh per frame: 16.7 ms at 60 Hz, which is two 120 Hz ticks (computed). Anything that shares that thread inherits the stall, including sim ticks, event pumping and ImGui. Handle a minimized window by skipping the frame, not by spinning.

### Vulkan FIFO blocking

**Area:** Vulkan FIFO blocking

**Library:** SDL3 SDL_GPU (Vulkan backend)

**Rule:** Two SDL issues bear on blocking:
- Issue 11293 (opened 2024-10-21, moved to the 3.x milestone on 2025-08-26, no linked fix): FIFO makes vkQueuePresentKHR block on Android, Wayland and NVIDIA on Windows. SDL has not moved present to a background thread.
- Issue 16548 (SDL 3.4.14, 2026-10-18): the non-blocking acquire passes UINT64_MAX to vkAcquireNextImageKHR. With Hyprland and the display off, both acquire variants block until the display returns. It was closed as a compositor bug.

**Source:** https://github.com/libsdl-org/SDL/issues/11293 ; https://github.com/libsdl-org/SDL/issues/16548

**Confidence:** documented

**Implication:** On Linux the platform thread can block in acquire or submit for longer than one vblank, and in rare compositor cases indefinitely. A sim cadence that must survive this cannot live on the platform thread. MAILBOX avoids FIFO waits where it is supported.

### Frames in flight

**Area:** Frames in flight

**Library:** SDL3 SDL_GPU

**Rule:** SDL_SetGPUAllowedFramesInFlight defaults to 2 and accepts 1 to 3. 'Higher values increase throughput at the expense of visual latency.' Calling it 'will stall and flush the command queue'.

**Source:** https://wiki.libsdl.org/SDL3/SDL_SetGPUAllowedFramesInFlight ; include/SDL3/SDL_gpu.h

**Confidence:** documented

**Implication:** Set it once at startup. One frame in flight gives the lowest latency but makes CPU recording wait for the GPU.

### GPU resources and uploads

**Area:** GPU resources and uploads

**Library:** SDL3 SDL_GPU

**Rule:** What is and is not documented:
- No thread-safety statement exists for SDL_CreateGPUTexture, SDL_CreateGPUBuffer, SDL_CreateGPUTransferBuffer, SDL_MapGPUTransferBuffer or SDL_ReleaseGPU*.
- SDL_SetGPUTextureName and SDL_SetGPUBufferName are 'not thread safe, you must make sure the texture is not simultaneously used by any other thread'.
- SDL_ReleaseGPUTexture 'Frees the given texture as soon as it is safe to do so'.
- Transfer buffers 'effectively function as ring buffers on internal resources' through cycling.
- Fences can be polled or waited on 'in a thread'.
In the source, the backends guard allocation and disposal with mutexes: Vulkan allocatorLock and disposeLock; D3D12 disposeLock, fenceLock and the staging-pool locks.

**Source:** include/SDL3/SDL_gpu.h ; https://wiki.libsdl.org/SDL3/CategoryGPU ; src/gpu/vulkan/SDL_gpu_vulkan.c ; src/gpu/d3d12/SDL_gpu_d3d12.c

**Confidence:** inferred

**Implication:** An asset thread can probably create textures, record its own copy pass, submit with SDL_SubmitGPUCommandBufferAndAcquireFence and publish the handle once the fence signals. That conclusion is inferred from the locks; it is not documented. Data in CPU snapshots is copied into transfer buffers at map time, so a snapshot can be recycled after the upload is recorded. A texture handle must not be released while any pending snapshot still names it.

### SDL_Renderer fallback

**Area:** SDL_Renderer fallback

**Library:** SDL3 SDL_Renderer

**Rule:** 'These functions must be called from the main thread.' SDL_RenderPresent 'should only be called on the main thread.'

**Source:** https://wiki.libsdl.org/SDL3/CategoryRender ; https://wiki.libsdl.org/SDL3/SDL_RenderPresent

**Confidence:** documented

**Implication:** An SDL_Renderer fallback can never run on a render thread. If the renderer interface must support it, every backend path has to be callable from the platform thread.

### Keyboard, mouse and gamepad state

**Area:** Keyboard, mouse and gamepad state

**Library:** SDL3

**Rule:** SDL_GetKeyboardState is safe to call from any thread, but its array 'may change without warning or any sort of thread synchronization' as events are pumped. SDL_GetMouseState and SDL_GetGlobalMouseState are main-thread only. SDL_GetGamepadButton and SDL_UpdateGamepads are safe from any thread, and the event loop updates gamepads automatically.

**Source:** https://wiki.libsdl.org/SDL3/SDL_GetKeyboardState ; https://wiki.libsdl.org/SDL3/SDL_GetMouseState ; https://wiki.libsdl.org/SDL3/SDL_GetGlobalMouseState ; https://wiki.libsdl.org/SDL3/SDL_GetGamepadButton ; https://wiki.libsdl.org/SDL3/SDL_UpdateGamepads

**Confidence:** documented

**Implication:** The sim thread must never read SDL input state directly. The platform thread latches events into an InputFrame (held state plus edges since the previous tick) and hands it over by value. Reading gamepads from another thread is safe but would break replay determinism.

### Event timestamps

**Area:** Event timestamps

**Library:** SDL3

**Rule:** SDL_CommonEvent.timestamp is 'In nanoseconds, populated using SDL_GetTicksNS()'. On Windows, WIN_GetEventTimestamp builds the timestamp from the Win32 message tick, which is in milliseconds, offsets it into SDL time and clamps it to now. Raw-input mouse events are spread across the poll interval.

**Source:** https://wiki.libsdl.org/SDL3/SDL_CommonEvent ; https://github.com/libsdl-org/SDL/blob/main/src/video/windows/SDL_windowsevents.c (WIN_GetEventTimestamp, WIN_PollRawInput)

**Confidence:** verified-in-source

**Implication:** On Windows, timestamps place key edges on 120 Hz ticks with millisecond resolution at best; the real resolution of the underlying tick was not checked. Edges latched at pump time are the dependable unit for InputFrames.

### Clocks and sleeps

**Area:** Clocks and sleeps

**Library:** SDL3

**Rule:** SDL_GetTicksNS and SDL_DelayPrecise are safe from any thread.
- Clocks: SDL_GetTicksNS uses QueryPerformanceCounter on Windows and CLOCK_MONOTONIC_RAW on Linux.
- Windows sleep: SDL_DelayNS waits on a per-thread (TLS) waitable timer created with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, falling back to an event wait and then to Sleep. It never calls timeBeginPeriod.
- Unix sleep: a nanosleep loop that retries on EINTR.
- SDL_DelayPrecise: sleeps in 1 ms steps while tracking the largest overshoot, sleeps the remainder minus that overshoot, then spins with SDL_CPUPauseInstruction until the target.

**Source:** https://wiki.libsdl.org/SDL3/SDL_GetTicksNS ; https://wiki.libsdl.org/SDL3/SDL_DelayPrecise ; https://github.com/libsdl-org/SDL/blob/main/src/timer/SDL_timer.c ; src/timer/windows/SDL_systimer.c ; src/timer/unix/SDL_systimer.c

**Confidence:** verified-in-source

**Implication:** A 120 Hz tick thread can pace itself to an absolute deadline with SDL_DelayPrecise, without process-wide timer changes. The spin tail is about 1 ms or less per wait, at most about 12% of one core at 120 Hz (computed upper bound). Precision depends on 1 ms sleeps being accurate, which needs Windows 10 1803 or later.

### ImGui context

**Area:** ImGui context

**Library:** Dear ImGui (vendored folder 1.92.5; imgui.h reports 1.92.6 WIP)

**Rule:** 'Dear ImGui functions are not thread-safe because of this pointer'. GImGui is a plain global, and the only supported multithreaded form is one context per thread through a thread_local GImGui. In 1.92, font functions such as CalcTextSizeA are 'definitively not thread-safe anymore as new glyphs may be loaded'.

**Source:** dep/imgui-1.92.5/imgui.cpp:1405-1416 ; dep/imgui-1.92.5/imgui.cpp:472-473

**Confidence:** verified-in-source

**Implication:** Only one thread at a time may drive a context. That covers ImGui_ImplSDL3_ProcessEvent, NewFrame through Render, and every widget and text-measure call. No ImGui calls are allowed from jobs or from the sim thread.

### ImGui platform backend

**Area:** ImGui platform backend

**Library:** imgui_impl_sdl3 (vendored)

**Rule:** ImGui_ImplSDL3_NewFrame calls these SDL functions, several of which are documented as main-thread only:
- SDL_GetWindowSize / SDL_GetWindowSizeInPixels;
- SDL_GetDisplays (every frame on Windows);
- SDL_CaptureMouse, SDL_GetKeyboardFocus, SDL_WarpMouse*, SDL_GetMouseFocus, SDL_GetGlobalMouseState and SDL_SetCursor;
- SDL_StartTextInput / SDL_SetTextInputArea;
- SDL_GetGamepads / SDL_OpenGamepad.

**Source:** dep/imgui-1.92.5/backends/imgui_impl_sdl3.cpp:970-1014, 712-760, 805, 860-872, 923, 192-224

**Confidence:** verified-in-source

**Implication:** The editor's ImGui frame must run on the SDL main thread. Only the GPU replay of its draw data can move to another thread, and only through a snapshot.

### ImGui draw data

**Area:** ImGui draw data

**Library:** Dear ImGui

**Rule:** GetDrawData is 'Valid after Render() and until the next call to NewFrame()'. ImDrawData::CmdLists are 'owned by ImGuiContext and only pointed to from here'. ImDrawList::CloneOutput exists, but the header points multithreaded renderers to imgui_club's imgui_threaded_rendering, which is not vendored here.

**Source:** dep/imgui-1.92.5/imgui.cpp:5228 ; dep/imgui-1.92.5/imgui.h:3541, 3545, 3486 ; dep/imgui-1.92.5/imgui_draw.cpp:496 ; dep/imgui-1.92.5/docs/TODO.txt:38

**Confidence:** verified-in-source

**Implication:** A render thread can replay ImGui only if the UI thread waits for it before the next NewFrame, which serializes the two threads, or if the UI thread hands it a snapshot.

### ImGui snapshot helper

**Area:** ImGui snapshot helper

**Library:** imgui_club imgui_threaded_rendering v0.20

**Rule:** What the helper provides:
- ImDrawDataSnapshot::SnapUsingSwap swaps each draw list's buffers into a cached copy ('src' becomes unusable), keeps capacity between frames and frees entries unused for 20 s.
- Its texture queue acts as a proxy backend on the update thread: it acknowledges texture requests immediately and shares tex->Pixels without copying.
- Each ImDrawData::Textures pointer must be nulled before the backend's render call.
- It assumes one context per atlas and does not handle device loss.
- Destroys are retired on a consumer-side clock, because update and render rates can differ.

**Source:** https://github.com/ocornut/imgui_club/tree/main/imgui_threaded_rendering

**Confidence:** documented

**Implication:** Rendering the editor UI on another thread is feasible with this helper plus one more vendored file. Device loss (SDL_EVENT_RENDER_DEVICE_LOST) needs its own handling path.

### ImGui textures (1.92)

**Area:** ImGui textures (1.92)

**Library:** Dear ImGui

**Rule:** How texture requests flow:
- draw_data->Textures points at the live PlatformIO.Textures, and that list is rebuilt every EndFrame.
- In NewFrame, ImFontAtlasUpdateNewFrame rewrites texture statuses, queues destroys and deletes textures that the backend marked Destroyed.
- Backends must call SetTexID and SetStatus after honoring requests.
- The pointer lives in ImDrawData so that multithreaded renderers can replace it.
- RendererHasTextures support 'will likely be REQUIRED for all backends before June 2026'.

**Source:** dep/imgui-1.92.5/imgui.cpp:6007, 9485-9498 ; dep/imgui-1.92.5/imgui_draw.cpp:2752-2849 ; dep/imgui-1.92.5/imgui.h:3637-3640 ; dep/imgui-1.92.5/docs/BACKENDS.md:300, 325

**Confidence:** verified-in-source

**Implication:** Texture requests are UI-thread state. A render thread that walks the live list races the next NewFrame, so requests must be processed on the UI thread or passed through a queue.

### ImGui SDL_GPU renderer

**Area:** ImGui SDL_GPU renderer

**Library:** imgui_impl_sdlgpu3 (vendored)

**Rule:** Contract of the vendored backend:
- PrepareDrawData is mandatory before the render pass, because copy passes cannot run inside a render pass. It also processes draw_data->Textures.
- UpdateTexture acquires and submits its own command buffer on the calling thread.
- One vertex/index buffer pair serves 'rendering 1 current in-flight frame' and relies on cycle=true. Growing it calls SDL_WaitForGPUIdle.
- RenderDrawData writes PlatformIO.Renderer_RenderState into the context.
- ImTextureID is SDL_GPUTexture*.

**Source:** dep/imgui-1.92.5/backends/imgui_impl_sdlgpu3.h:5, 21-23 ; dep/imgui-1.92.5/backends/imgui_impl_sdlgpu3.cpp:47, 136, 155-157, 167-170, 184-185, 214-215, 237-242, 400-404

**Confidence:** verified-in-source

**Implication:** Whichever thread calls PrepareDrawData also performs ImGui texture uploads and must own the command buffer it passes in. Buffer growth stalls the whole GPU, though rarely. RenderDrawData touches the context, so running it alongside the next NewFrame requires the snapshot approach.

### ImGui multi-viewports

**Area:** ImGui multi-viewports

**Library:** Dear ImGui + imgui_impl_sdl3 + imgui_impl_sdlgpu3

**Rule:** UpdatePlatformWindows creates and destroys secondary windows through SDL_CreateWindow and SDL_ClaimWindowForGPUDevice. RenderPlatformWindowsDefault calls the sdlgpu3 RenderWindow, which acquires a command buffer and a swapchain texture and submits on the calling thread. The SDL3 backend notes that the OS animation when windows are created and destroyed is a problem.

**Source:** dep/imgui-1.92.5/imgui.h:1181-1182 ; dep/imgui-1.92.5/imgui.cpp:17230-17267, 17342-17360 ; dep/imgui-1.92.5/backends/imgui_impl_sdl3.cpp:11, 1075 ; dep/imgui-1.92.5/backends/imgui_impl_sdlgpu3.cpp:697-734

**Confidence:** verified-in-source

**Implication:** With viewports enabled, secondary windows are created, rendered and presented on the platform thread inside the UI frame. Keep multi-viewports opt-in.

### ECS registry

**Area:** ECS registry

**Library:** EnTT 3.16.0 (vendored)

**Rule:** 'the entire registry is not thread safe' and 'users should not try to iterate a set of components and modify the same set concurrently'. Another thread may work on different components at the same time. A single set may be iterated by many threads 'as long as the components are neither assigned nor removed in the meantime'.

**Source:** dep/entt-3.16.0/docs/md/entity.md:2333-2368

**Confidence:** verified-in-source

**Implication:** Structural changes happen on one thread at a time. Parallel work is limited to read-only iteration, or to writes partitioned by entity while nothing is assigned or removed.

### ECS lazy storage

**Area:** ECS lazy storage

**Library:** EnTT 3.16.0

**Rule:** The non-const assure<T>() calls pools.emplace when a storage is missing, so any non-const view<T>() or storage<T>() can change the pools map. The const overload returns nullptr instead. 'A const registry is also fully thread safe', but views made from one 'may contain dangling references to non-existing storage'.

**Source:** dep/entt-3.16.0/src/entt/entity/registry.hpp:248-276, 280-294 ; dep/entt-3.16.0/docs/md/entity.md:2410-2433

**Confidence:** verified-in-source

**Implication:** Create every storage when the World is built, with registry.storage<T>() or the organizer's prepare(). Give workers a const registry& or prebuilt views, so that no worker can trigger a hidden structural change.

### ECS owning groups

**Area:** ECS owning groups

**Library:** EnTT 3.16.0

**Rule:** A group handler connects to on_construct and on_destroy of every pool in the group, and swaps owned elements to keep them packed.

**Source:** dep/entt-3.16.0/src/entt/entity/group.hpp:106-115, 147-148

**Confidence:** verified-in-source

**Implication:** The 'different components may be touched concurrently' exception does not hold across an owning group. Adding or removing Sprite reorders the MovementPattern and EntityStatus pools of the group at core/system/WayPointSystem.cpp:14 (inferred consequence).

### ECS signals

**Area:** ECS signals

**Library:** EnTT 3.16.0

**Rule:** on_construct, on_update and on_destroy listeners are invoked synchronously inside the call that mutates the registry.

**Source:** dep/entt-3.16.0/src/entt/entity/mixin.hpp:79-112, 295-337

**Confidence:** verified-in-source

**Implication:** A worker that mutates the registry runs every listener on that worker, including group handlers and editor bookkeeping.

### ECS task graph

**Area:** ECS task graph

**Library:** EnTT organizer

**Rule:** The organizer builds an adjacency-list graph from function signatures, and constness decides read-only or read-write access. 'All functions have the registry among their resources', and a non-const registry& parameter means read-write on the registry. The organizer does not execute tasks. It offers vertex prepare() 'Since the creation of pools and resources within the registry is not necessarily thread safe'.

**Source:** dep/entt-3.16.0/docs/md/entity.md:881-990

**Confidence:** verified-in-source

**Implication:** The organizer can derive which systems may run in parallel, but only if systems take views and const context instead of registry&. Any system that takes registry& serializes against every other system.

### ECS build config

**Area:** ECS build config

**Library:** EnTT 3.16.0

**Rule:** Define ENTT_USE_ATOMIC 'unless you know exactly what you are doing', even when each thread only uses local data. It makes the static type-index and family counters atomic.

**Source:** dep/entt-3.16.0/docs/md/entity.md:2370-2372 ; dep/entt-3.16.0/docs/md/config.md:43-50 ; dep/entt-3.16.0/src/entt/config/config.h:32-36 ; src/entt/core/type_info.hpp:18 ; src/entt/core/family.hpp:19

**Confidence:** verified-in-source

**Implication:** This is a required compile definition for any threaded use of EnTT.

### ECS entity identity

**Area:** ECS entity identity

**Library:** EnTT 3.16.0

**Rule:** With a uint32 backing type, entt::entity is a 20-bit index (0xFFFFF) plus a 12-bit version (0xFFF). next() increments the version and skips the tombstone value, so the version wraps to 0 after 4095 reuses of a slot.

**Source:** dep/entt-3.16.0/src/entt/entity/entity.hpp:32-39, 116-118

**Confidence:** verified-in-source

**Implication:** Match entities across two snapshots by the full 32-bit id, so a recycled slot is never interpolated. Version wrap allows ABA after 4095 reuses of one slot, so references held across many snapshots or saves should use the Guid.

### ECS snapshot copies

**Area:** ECS snapshot copies

**Library:** EnTT 3.16.0

**Rule:** basic_registry and basic_storage are not copyable. entt::snapshot is an archive serializer. Component storage is paged (ENTT_PACKED_PAGE 1024) with raw() page access. Single-type view iterators are random access; multi-type views are at least forward iterators.

**Source:** dep/entt-3.16.0/src/entt/entity/registry.hpp:355, 376 ; dep/entt-3.16.0/src/entt/entity/storage.hpp:447, 543-550 ; dep/entt-3.16.0/src/entt/config/config.h:50-51 ; dep/entt-3.16.0/docs/md/entity.md:1089-1145, 2376-2400

**Confidence:** verified-in-source

**Implication:** A render or interpolation snapshot is an explicit copy of the needed fields into flat arrays, made in one pass. To split that copy across workers, divide index ranges of the leading storage; multi-type view iterators cannot be split that way.

### Windows timer resolution

**Area:** Windows timer resolution

**Library:** Windows (winmm)

**Rule:** Since Windows 10 version 2004, timeBeginPeriod 'no longer affects global timer resolution' and applies per process. 'Starting with Windows 11, if a window-owning process becomes fully occluded, minimized, or otherwise invisible or inaudible to the end user, Windows does not guarantee a higher resolution than the default system resolution.' A higher resolution 'can also prevent the CPU power management system from entering power-saving modes'. The Sleep documentation advises calling timeBeginPeriod once, early.

**Source:** https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod ; https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleep ; https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessinformation

**Confidence:** documented

**Implication:** Never toggle the timer period per frame. Expect pacing to degrade while the window is minimized or occluded, which is acceptable if the sim pauses or throttles there.

### Windows high-resolution waits

**Area:** Windows high-resolution waits

**Library:** Windows

**Rule:** CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is for 'time-critical situations when short expiration delays on the order of a few milliseconds are unacceptable. This value is supported in Windows 10, version 1803, and later.' Sleep may wake anywhere between one and two clock ticks for an interval between them, and may return early for an interval shorter than one tick.

**Source:** https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw ; https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleep

**Confidence:** documented

**Implication:** Use this timer as the wait primitive for a tick thread; SDL_DelayNS already uses it. Plain Sleep or sleep_for is not precise enough for an 8.33 ms period.

### Windows QoS and hybrid CPUs

**Area:** Windows QoS and hybrid CPUs

**Library:** Windows 11

**Rule:** QoS levels:
- High: in focus, audible, or explicitly tagged.
- Medium: visible but not in focus; also the foreground app after user inactivity on battery.
- Low: not visible or audible; 'On battery, selects most efficient CPU frequency and schedules to efficient core'.
- Eco: explicit only; 'Always selects most efficient CPU frequency and schedules to efficient cores'.
On heterogeneous processors, QoS 'may restrict scheduling to a subset of processors', and reduced thread priority can imply a lower QoS. SetThreadInformation(ThreadPowerThrottling) with ControlMask=THREAD_POWER_THROTTLING_EXECUTION_SPEED and StateMask=0 opts a thread into HighQoS; setting both masks to EXECUTION_SPEED makes it EcoQoS.

**Source:** https://learn.microsoft.com/en-us/windows/win32/procthread/quality-of-service ; https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadinformation ; https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessinformation

**Confidence:** documented

**Implication:** On Intel hybrid laptops, an editor left visible but unfocused, or a game running on battery, can have its sim and render threads moved to E-cores or low clocks. Tag time-critical threads HighQoS and never lower their priority. Tag asset and log workers EcoQoS.

### CPU topology

**Area:** CPU topology

**Library:** Windows

**Rule:** GetSystemCpuSetInformation reports EfficiencyClass, where 'higher numerical values ... are faster but less power-efficient'. It also reports CoreIndex, which SMT siblings share, and LastLevelCacheIndex. SetThreadSelectedCpuSets gives a thread soft affinity.

**Source:** https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-system_cpu_set_information

**Confidence:** documented

**Implication:** Size worker pools from P-cores and physical cores at startup instead of hardware_concurrency(). Keep fork-join workers on one last-level cache; Box2D gives the same advice.

### Windows priority boost

**Area:** Windows priority boost

**Library:** Windows MMCSS

**Rule:** Threads that join the 'Games' task with AvSetMmThreadCharacteristics get boosted priority: the Medium category, priority 16-22, covers foreground applications. SystemResponsiveness reserves CPU for low-priority work; values below 10 or above 100 are clamped to 20.

**Source:** https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service

**Confidence:** documented

**Implication:** This is an optional boost for the platform and sim threads. It does not guarantee any deadline.

### Deadlocks with the window thread

**Area:** Deadlocks with the window thread

**Library:** Windows DXGI

**Rule:** 'Be careful that you never have the message-pump thread wait on the render thread when you use full-screen swap chains.' Present on a render thread can call SendMessage to the pump thread during a mode change, and the two threads deadlock if the pump thread is waiting.

**Source:** https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi (Multithread considerations)

**Confidence:** documented

**Implication:** General rule for any design: the window thread must never block on a thread that may itself wait on the window thread. Examples are DXGI present, SDL_RunOnMainThread with wait=true, and SendMessage. Because SDL_GPU presents only on the window thread, the DXGI case itself cannot arise.

### Linux sleeps and scheduler

**Area:** Linux sleeps and scheduler

**Library:** Linux kernel / glibc

**Rule:** Linux sleep and scheduling behaviour:
- nanosleep rounds the interval up to the clock granularity, and the thread may still wait for a CPU afterwards. Linux measures it against CLOCK_MONOTONIC.
- Use clock_nanosleep with TIMER_ABSTIME to avoid drift.
- High-resolution timers make sleeps 'as accurate as the hardware allows (microsecond accuracy is typical...)'.
- Timer slack delays wake-ups. init uses 50 us and new threads inherit the value; slack is not applied under real-time policies.
- The transition to the EEVDF scheduler began in kernel 6.6; sched_setattr can request time slices.

**Source:** https://man7.org/linux/man-pages/man2/nanosleep.2.html ; https://man7.org/linux/man-pages/man7/time.7.html ; https://man7.org/linux/man-pages/man2/PR_SET_TIMERSLACK.2const.html ; https://docs.kernel.org/scheduler/sched-eevdf.html

**Confidence:** documented

**Implication:** Absolute-deadline sleeps plus a short spin hold 120 Hz on Linux. SDL's relative nanosleep accumulates drift, which SDL_DelayPrecise's spin hides. Real-time scheduling needs privileges and is not assumed.

### Physics API thread rules

**Area:** Physics API thread rules

**Library:** Box2D v3

**Rule:** Rules from the Box2D documentation:
- Do not read or write the world during b2World_Step.
- Do not write to one world from several threads.
- Ray casts, shape casts and overlap tests are safe from multiple threads outside b2World_Step.
- Several worlds may run on different threads, but creating or destroying worlds from several threads needs a mutex.
- Connected callbacks, such as allocators, must be thread-safe.
- Multithreading is data-parallel, and other engine threads should not compete for cores during the step.

**Source:** https://box2d.org/documentation/md_foundation.html

**Confidence:** documented

**Implication:** The renderer and editor read a copy taken after the step, never the live world during a step. Queries from other threads run only between steps.

### Physics task system

**Area:** Physics task system

**Library:** Box2D v3.1 / v3.2

**Rule:** v3.1 supports only a user task system ('Box2D does not create threads'), with parallel-for callbacks taking itemCount, minRange and a workerIndex in [0, workerCount). v3.2.0 (19 Oct) adds a native task system that 'runs its own threads when no task callbacks are hooked up' and changes the callback types to b2TaskCallbackFcn(void* taskContext). On workerCount: 'Box2D performs best when using performance cores and accessing a single L3 cache ... Efficiency cores and SMT provide little benefit and may even harm performance.'

**Source:** https://raw.githubusercontent.com/erincatto/box2d/v3.1.0/include/box2d/types.h ; https://github.com/erincatto/box2d/blob/main/include/box2d/types.h ; https://github.com/erincatto/box2d/releases

**Confidence:** verified-in-source

**Implication:** Pin one version. Feed Box2D's enqueue and finish callbacks from the engine's single worker pool, so that Box2D does not start a second pool.

### Physics determinism

**Area:** Physics determinism

**Library:** Box2D v3

**Rule:** The author reports three levels of determinism: algorithmic, multithreaded (independent of thread count) and cross-platform (since 3.1). They were tested on x64 and ARM with MSVC, GCC and Clang. This relies on custom sin, cos and atan2, no fast-math, and FMA contraction disabled (-ffp-contract=off on GCC and Clang). CI checks a hash of body transforms.

**Source:** https://box2d.org/posts/2024/08/determinism/ ; https://github.com/erincatto/box2d/releases

**Confidence:** documented

**Implication:** Box2D does not by itself break per-tick determinism under threading. First-party sim code needs the same floating-point discipline before any golden hash can hold across compilers.

### Physics events and body types

**Area:** Physics events and body types

**Library:** Box2D v3

**Rule:** Sensor and contact events 'are available after every call to b2World_Step()' and should be processed before other game logic. Sensor events are off by default, and both shapes need enableSensorEvents. Kinematic bodies 'do not collide with other kinematic or static bodies'. The character mover in 3.2 is experimental. Its geometric mover lives outside the world and is recommended for rollback; the alternative is a dynamic-body mover.

**Source:** https://box2d.org/documentation/md_simulation.html ; https://github.com/erincatto/box2d/blob/main/include/box2d/types.h ; https://box2d.org/documentation/md_character.html

**Confidence:** documented

**Implication:** Events are consumed on the stepping thread right after the step. Brawler characters need the mover or first-party collision resolution. Box2D's threading lever is parallelism inside the step; it does not by itself split simulation from rendering.

### Audio callback thread

**Area:** Audio callback thread

**Library:** miniaudio

**Rule:** Rules from the miniaudio manual:
- ma_device_init, uninit, start and stop 'must never be called inside the callback'; starting or stopping there deadlocks.
- Node processing callbacks 'are always run on the audio thread'.
- Only ma_node_graph_read_pcm_frames is lock-free. Attach and detach use spinlocks, and detach stalls until the audio thread is done.
- Sound-end callbacks fire on the audio thread.
- Ring buffers are lock-free only with exactly one producer and one consumer.
- The resource manager runs one job thread by default, with a spinlock job queue. It can also run with zero job threads and manual ma_job_process.
- The manual gives no thread-safety guarantee for ma_engine or ma_sound calls.

**Source:** https://miniaud.io/docs/manual/index.html (sections 1.1, 5, 6.2, 7.1, 7.2, 14)

**Confidence:** documented

**Implication:** miniaudio's own device thread is the only audio thread the engine needs. Game code issues ma_sound commands from one owner thread, for example the platform thread after the step, draining audio events. The audio thread never reads world state.

### Profiling annotations

**Area:** Profiling annotations

**Library:** Tracy

**Rule:** Profiling rules:
- Name threads with tracy::SetThreadName.
- Recording an event costs a few nanoseconds; the manual's benchmark gives 2.25 ns per zone.
- FrameMarkNamed creates separate frame sets.
- FrameMarkStart and FrameMarkEnd mark discontinuous frames and must not be mixed with continuous marks under the same name.
- TracyLockable, TracySharedLockable and LockMark annotate mutexes.
- Fibers are opt-in and cost extra during capture.
- A C API zone 'must end in the same thread in which it was started'.
- TRACY_ON_DEMAND collects data only while a client is connected.

**Source:** https://github.com/wolfpld/tracy/blob/master/manual/tracy.tex

**Confidence:** documented

**Implication:** Name every engine thread. Use one frame set for sim ticks and another for rendered frames. Wrap any shared mutex so that contention shows up in captures.

### Logging

**Area:** Logging

**Library:** spdlog 1.x

**Rule:** Thread safety and async logging:
- _mt loggers and sinks are thread-safe.
- set_error_handler and logger::sinks() must not be used concurrently on any logger.
- Async logging uses one global pool: 8192 preallocated slots of about 256 B each, and one worker thread.
- On overflow the caller blocks by default; overrun_oldest drops the oldest message instead.
- More than one worker can reorder messages.
- On Windows, call spdlog::shutdown() before main returns, to avoid a Visual Studio runtime deadlock at exit.
README benchmark (i7-4770, Ubuntu):
- basic_st: 5,777,626 msg/s.
- basic_mt with 10 threads: 1,659,613/s.
- async, block policy: 585,535/s.
- async, overrun policy: 2,682,285/s.

**Source:** https://github.com/gabime/spdlog/wiki/Thread-Safety ; https://github.com/gabime/spdlog/wiki/Asynchronous-logging ; https://github.com/gabime/spdlog (README benchmarks)

**Confidence:** documented

**Implication:** Hot-path logging from several threads should go through an async logger with overrun_oldest and one worker, so it never blocks the sim. Configure sinks before any thread starts, and call spdlog::shutdown() in the teardown sequence.

### C++20/23 concurrency library

**Area:** C++20/23 concurrency library

**Library:** MSVC STL 14.51 (VS 18) / libstdc++

**Rule:** MSVC 14.51.36231 (_MSVC_STL_UPDATE 202604) defines these feature-test macros:
- __cpp_lib_jthread 201911, atomic_wait 201907, latch 201907, barrier 202302, semaphore 201907;
- atomic_shared_ptr 201711, atomic_ref 201806, hardware_interference_size 201703;
- move_only_function 202110, expected 202211, flat_map 202511.
MSVC's atomic wait uses WaitOnAddress for 1-, 2-, 4- and 8-byte objects.
First libstdc++ release for each:
- jthread: GCC 10.1.
- Synchronization library (atomic wait, latch, barrier, counting_semaphore): 11.1.
- atomic<shared_ptr>: 12.1.
- Interference sizes: 12.1.
- flat_map: 15.1.

**Source:** C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/include/yvals_core.h:905-906, 1629-1770 ; .../include/xatomic_wait.h:25-29 ; https://learn.microsoft.com/en-us/cpp/overview/visual-cpp-language-conformance ; https://gcc.gnu.org/onlinedocs/libstdc++/manual/status.html

**Confidence:** verified-in-source

**Implication:** jthread with stop_token, latch, barrier, counting_semaphore and atomic wait/notify are all usable on both targets with GCC 12 or later.

### atomic<shared_ptr>

**Area:** atomic<shared_ptr>

**Library:** MSVC STL / libstdc++

**Rule:** atomic<shared_ptr<T>>::is_always_lock_free is false on MSVC, which uses a locked control-block pointer. It is also false on libstdc++, which uses the low bit of the control-block pointer as a spin lock.

**Source:** C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/include/memory:4093-4100 ; https://github.com/gcc-mirror/gcc/blob/master/libstdc%2B%2B-v3/include/bits/shared_ptr_atomic.h

**Confidence:** verified-in-source

**Implication:** It is not a wait-free way to hand off snapshots, and the reader can spin behind the writer. Publish through an atomic index or pointer to engine-owned buffers instead.

### False sharing

**Area:** False sharing

**Library:** MSVC / GCC

**Rule:** On MSVC, hardware_destructive_interference_size is 64. GCC derives the value from -mtune and warns by default (-Winterference-size) that a header needing ABI stability 'should probably not use the hardware interference size variables at all'.

**Source:** C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/include/new:41-42 ; https://gcc.gnu.org/onlinedocs/gcc/Warning-Options.html

**Confidence:** verified-in-source

**Implication:** Pad counters shared between threads with an engine constant (64), not the std constant, in any public header.

### Thread lifetime

**Area:** Thread lifetime

**Library:** C++20 standard library

**Rule:** The jthread destructor requests stop and then joins, and the thread function can take a stop_token. condition_variable_any::wait(lock, stop_token, pred) returns when stop is requested. atomic::wait returns only after the value has changed, but 'transient changes from old to another value and back to old might be missed'.

**Source:** https://en.cppreference.com/w/cpp/thread/jthread ; https://en.cppreference.com/w/cpp/thread/condition_variable_any/wait ; https://en.cppreference.com/w/cpp/atomic/atomic/wait

**Confidence:** documented

**Implication:** With jthread, shutdown order can follow member declaration order. Every blocking wait must be stop-aware. Wait on monotonically increasing sequence numbers, such as tick counters, not on flags that toggle back and forth.

### Legacy GL and window rules

**Area:** Legacy GL and window rules

**Library:** SFML 3.0.2 (vendored, reference only)

**Rule:** What the vendored SFML does:
- A window's GL context is active on only one thread at a time and must be deactivated before another thread activates it. The current context is thread_local.
- GL resources created without an active context use a TransientContext that locks the shared context's mutex.
- setMouseCursorVisible 'needs to be called from the thread that created the window' on Windows.
- Win32 events are pumped with PeekMessageW(nullptr), which serves only the calling thread.
- setFramerateLimit sleeps by calling timeBeginPeriod, Sleep(whole ms) and timeEndPeriod on every frame.

**Source:** dep/SFML-3.0.2/include/SFML/Window/Window.hpp:317-322 ; dep/SFML-3.0.2/src/SFML/Window/GlContext.cpp:182, 321-350 ; dep/SFML-3.0.2/include/SFML/Window/WindowBase.hpp:442-443 ; dep/SFML-3.0.2/src/SFML/Window/Win32/WindowImplWin32.cpp:305-317 ; dep/SFML-3.0.2/src/SFML/Window/Window.cpp:203-205 ; dep/SFML-3.0.2/src/SFML/System/Win32/SleepImpl.cpp:37-55

**Confidence:** verified-in-source

**Implication:** Reference only. The legacy tree's thread split is legal for GL contexts and events. It is not legal for ImGui-SFML's cursor calls or for sharing the window's view between threads.

## Thread Affinity

### Item 1

**Resource:** SDL video subsystem (SDL_Init with SDL_INIT_VIDEO); defines SDL's 'main thread'

**Must Run On:** On Windows and Linux, the one thread chosen as the platform thread (any thread may be chosen). On Apple, the process main() thread.

**May Move To:** Nowhere after init. Every video call is then pinned to that thread.

**Source:** https://wiki.libsdl.org/SDL3/SDL_Init ; https://wiki.libsdl.org/SDL3/SDL_IsMainThread

### Item 2

**Resource:** Windows: SDL_CreateWindow / SDL_DestroyWindow and window queries and setters (SDL_GetWindowSizeInPixels and similar)

**Must Run On:** SDL main thread

**May Move To:** Nowhere. Other threads use SDL_RunOnMainThread or values passed in a per-frame packet.

**Source:** https://wiki.libsdl.org/SDL3/SDL_CreateWindow ; https://wiki.libsdl.org/SDL3/SDL_GetWindowSizeInPixels

### Item 3

**Resource:** Event pump (SDL_PumpEvents, SDL_PollEvent, SDL_WaitEvent) and the Win32 message queue of the window

**Must Run On:** SDL main thread. This is also the thread that created the windows, which receives their Win32 messages.

**May Move To:** Nowhere. Any thread may call SDL_PushEvent or SDL_RunOnMainThread to reach it.

**Source:** https://wiki.libsdl.org/SDL3/SDL_PollEvent ; https://learn.microsoft.com/en-us/windows/win32/winmsg/about-messages-and-message-queues

### Item 4

**Resource:** Keyboard and mouse state (SDL_GetKeyboardState array, SDL_GetMouseState, SDL_GetGlobalMouseState, SDL_CaptureMouse, SDL_SetCursor, text input and IME)

**Must Run On:** SDL main thread. The keyboard array can be read from any thread, but the read is unsynchronized.

**May Move To:** Copies only: an InputFrame passed by value to the sim thread.

**Source:** https://wiki.libsdl.org/SDL3/SDL_GetKeyboardState ; https://wiki.libsdl.org/SDL3/SDL_GetMouseState

### Item 5

**Resource:** Gamepad state (SDL_GetGamepadButton, SDL_GetGamepadAxis, SDL_UpdateGamepads)

**Must Run On:** Any thread (documented safe). The event loop on the main thread updates the state.

**May Move To:** Any thread. For replay determinism, sample it on the main thread into the InputFrame.

**Source:** https://wiki.libsdl.org/SDL3/SDL_GetGamepadButton ; https://wiki.libsdl.org/SDL3/SDL_UpdateGamepads

### Item 6

**Resource:** SDL_GPUDevice create and destroy, SDL_SetGPUAllowedFramesInFlight

**Must Run On:** Main thread at init and shutdown. No rule is documented; setting frames in flight stalls and flushes the queue.

**May Move To:** Unknown (undocumented).

**Source:** https://wiki.libsdl.org/SDL3/SDL_CreateGPUDevice ; https://wiki.libsdl.org/SDL3/SDL_SetGPUAllowedFramesInFlight

### Item 7

**Resource:** Window claim and swapchain setup (SDL_ClaimWindowForGPUDevice, SDL_SetGPUSwapchainParameters, SDL_ReleaseWindowFromGPUDevice)

**Must Run On:** The thread that created the window, which is the SDL main thread. The rule is documented for Claim; for SetGPUSwapchainParameters it is undocumented, and the vendored ImGui backend calls it on that same thread.

**May Move To:** Nowhere

**Source:** https://wiki.libsdl.org/SDL3/SDL_ClaimWindowForGPUDevice ; dep/imgui-1.92.5/backends/imgui_impl_sdlgpu3.cpp:697-703

### Item 8

**Resource:** Swapchain acquire and wait (SDL_AcquireGPUSwapchainTexture, SDL_WaitAndAcquireGPUSwapchainTexture, SDL_WaitForGPUSwapchain), plus the command buffer that presents (acquire, record, submit)

**Must Run On:** The thread that created the window, which is the SDL main thread

**May Move To:** Nowhere. The swapchain texture may only be referenced by the command buffer that acquired it.

**Source:** https://wiki.libsdl.org/SDL3/SDL_AcquireGPUSwapchainTexture ; https://wiki.libsdl.org/SDL3/SDL_WaitAndAcquireGPUSwapchainTexture ; https://wiki.libsdl.org/SDL3/SDL_AcquireGPUCommandBuffer

### Item 9

**Resource:** Offscreen GPU command buffers (render, copy and compute passes into textures)

**Must Run On:** The single thread that acquired the buffer, for its whole life (record and submit)

**May Move To:** Any thread (a render-recording thread or a loader thread), one buffer per thread. The engine orders submissions across threads.

**Source:** https://wiki.libsdl.org/SDL3/SDL_AcquireGPUCommandBuffer ; https://wiki.libsdl.org/SDL3/CategoryGPU

### Item 10

**Resource:** GPU resources (textures, buffers, transfer buffers, pipelines, samplers): create, map/unmap, release

**Must Run On:** Undocumented. The backends use internal locks, and the GPU side of a release is deferred until safe.

**May Move To:** Likely any thread (inferred), with engine-level lifetime coordination. SDL_SetGPUTextureName and SDL_SetGPUBufferName are not thread-safe while the object is in use.

**Source:** include/SDL3/SDL_gpu.h ; src/gpu/vulkan/SDL_gpu_vulkan.c ; src/gpu/d3d12/SDL_gpu_d3d12.c

### Item 11

**Resource:** GPU fences (SDL_QueryGPUFence, SDL_WaitForGPUFences)

**Must Run On:** Any thread ('poll or wait on in a thread')

**May Move To:** Any thread

**Source:** https://wiki.libsdl.org/SDL3/CategoryGPU

### Item 12

**Resource:** SDL_Renderer (every function, including SDL_RenderPresent)

**Must Run On:** SDL main thread

**May Move To:** Nowhere

**Source:** https://wiki.libsdl.org/SDL3/CategoryRender ; https://wiki.libsdl.org/SDL3/SDL_RenderPresent

### Item 13

**Resource:** Dear ImGui context: ImGui_ImplSDL3_ProcessEvent, NewFrame through Render, all widgets, font and text measurement

**Must Run On:** One thread at a time. With imgui_impl_sdl3 this must be the SDL main thread, because NewFrame calls main-thread-only SDL functions.

**May Move To:** Nowhere in practice

**Source:** dep/imgui-1.92.5/imgui.cpp:1405-1416 ; dep/imgui-1.92.5/backends/imgui_impl_sdl3.cpp:970-1014

### Item 14

**Resource:** ImGui platform windows (multi-viewports): UpdatePlatformWindows, and RenderPlatformWindowsDefault through sdlgpu3 RenderWindow

**Must Run On:** SDL main thread, inside the UI frame

**May Move To:** Nowhere. These calls create windows, claim them, acquire swapchains and submit.

**Source:** dep/imgui-1.92.5/imgui.h:1181-1182 ; dep/imgui-1.92.5/backends/imgui_impl_sdl3.cpp:1075 ; dep/imgui-1.92.5/backends/imgui_impl_sdlgpu3.cpp:697-734

### Item 15

**Resource:** Main-viewport ImGui draw data (ImDrawData, ImGui_ImplSDLGPU3_PrepareDrawData, ImGui_ImplSDLGPU3_RenderDrawData)

**Must Run On:** The ImGui thread, before the next NewFrame

**May Move To:** A render thread, but only through an ImDrawDataSnapshot (imgui_club, not vendored) with draw_data->Textures nulled and handled by its texture queue

**Source:** dep/imgui-1.92.5/imgui.cpp:5228 ; dep/imgui-1.92.5/imgui.h:3541-3550 ; https://github.com/ocornut/imgui_club/tree/main/imgui_threaded_rendering

### Item 16

**Resource:** ImGui texture requests (ImTextureData status and pixels, PlatformIO.Textures)

**Must Run On:** The ImGui thread, because NewFrame mutates and deletes them

**May Move To:** A render thread, only through a proxy queue such as the imgui_club texture queue

**Source:** dep/imgui-1.92.5/imgui_draw.cpp:2752-2849 ; dep/imgui-1.92.5/imgui.cpp:6007, 9485-9498 ; dep/imgui-1.92.5/docs/BACKENDS.md:325

### Item 17

**Resource:** EnTT structural changes: create, destroy, emplace, remove; a non-const view or storage on a missing type; group creation; ctx emplace; plus every signal listener they trigger

**Must Run On:** One owning thread at a time, with no concurrent readers of the affected pools or of the pools map

**May Move To:** No concurrent use. Workers return results, and the owner applies them.

**Source:** dep/entt-3.16.0/docs/md/entity.md:2333-2368 ; dep/entt-3.16.0/src/entt/entity/registry.hpp:248-276 ; group.hpp:147-148 ; mixin.hpp:79-112

### Item 18

**Resource:** EnTT component data of existing entities, with no structural change

**Must Run On:** The owning thread by default

**May Move To:** Fork-join workers, under three conditions. First, reads may come from many threads, and writes must be partitioned by entity range or touch disjoint component types that no owning group ties together. Second, all storages are pre-created. Third, the main thread joins before the next structural change.

**Source:** dep/entt-3.16.0/docs/md/entity.md:2343-2354, 2376-2400 ; dep/entt-3.16.0/src/entt/entity/group.hpp:106-115

### Item 19

**Resource:** EnTT const registry (const views, const storage access)

**Must Run On:** Any number of threads concurrently, while no thread mutates the registry

**May Move To:** Any thread

**Source:** dep/entt-3.16.0/docs/md/entity.md:2410-2433

### Item 20

**Resource:** Box2D world (b2World_Step, body and shape create/destroy, event retrieval)

**Must Run On:** One stepping thread, plus Box2D's workers during the step. Creating and destroying worlds across threads needs a mutex.

**May Move To:** Queries (ray casts, shape casts, overlaps) to any thread, outside the step only

**Source:** https://box2d.org/documentation/md_foundation.html

### Item 21

**Resource:** miniaudio device data callback and node-graph processing

**Must Run On:** miniaudio's audio thread (created by the backend)

**May Move To:** Nowhere. Never start, stop or uninit the device from the callback. Game-side ma_engine and ma_sound calls should come from one owner thread (inferred; undocumented).

**Source:** https://miniaud.io/docs/manual/index.html

### Item 22

**Resource:** Tracy zones and messages

**Must Run On:** Any thread. A zone begins and ends on the same thread unless fibers are used.

**May Move To:** Any thread. Name each thread with tracy::SetThreadName.

**Source:** https://github.com/wolfpld/tracy/blob/master/manual/tracy.tex

### Item 23

**Resource:** spdlog _mt loggers and sinks

**Must Run On:** Any thread for logging. set_error_handler and edits to the sinks() vector happen on one thread, before concurrent use.

**May Move To:** Any thread; async writes go to the spdlog pool thread

**Source:** https://github.com/gabime/spdlog/wiki/Thread-Safety ; https://github.com/gabime/spdlog/wiki/Asynchronous-logging

### Item 24

**Resource:** Time and waits (SDL_GetTicksNS, SDL_DelayNS, SDL_DelayPrecise; SDL's high-resolution waitable timer on Windows)

**Must Run On:** Any thread. SDL keeps the timer handle per thread in TLS.

**May Move To:** Any thread

**Source:** https://wiki.libsdl.org/SDL3/SDL_DelayPrecise ; https://github.com/libsdl-org/SDL/blob/main/src/timer/windows/SDL_systimer.c

### Item 25

**Resource:** Windows QoS, power throttling and thread priority

**Must Run On:** Set per thread with SetThreadInformation and SetThreadPriority, or per process. A window-owning process's default QoS follows the window's focus and visibility.

**May Move To:** Not applicable (a per-thread attribute)

**Source:** https://learn.microsoft.com/en-us/windows/win32/procthread/quality-of-service ; https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadinformation

### Item 26

**Resource:** Legacy SFML window (waitEvent/pollEvent, setMouseCursorVisible, setMouseCursor) and its GL context

**Must Run On:** Events and cursor calls: the window-creating thread on Win32. GL context: one thread at a time.

**May Move To:** GL context only, by calling setActive(false) on the old thread before setActive(true) on the new one

**Source:** dep/SFML-3.0.2/include/SFML/Window/WindowBase.hpp:442-443 ; dep/SFML-3.0.2/include/SFML/Window/Window.hpp:317-322 ; dep/SFML-3.0.2/src/SFML/Window/Win32/WindowImplWin32.cpp:305-317

## Todays Violations

### Item 1

**Rule:** An ImGui context is not thread-safe; one thread at a time (dep/imgui-1.92.5/imgui.cpp:1408).

**Where:** app/editor/Editor.cpp:117 (ImGui::SFML::ProcessEvent on the main thread, reached from core/Engine.cpp:109) against app/editor/Editor.cpp:136 and :177 (ImGui::SFML::Update/NewFrame and Render on the render thread, reached from core/Engine.cpp:208). ProcessEvent pushes into g.InputEventsQueue (dep/imgui-1.92.5/imgui.cpp:1680, 1836, 1891).

**Consequence:** The main thread can push_back and reallocate the input queue while NewFrame consumes it on the render thread. That is a data race and undefined behaviour, with lost or garbled input and possible crashes.

### Item 2

**Rule:** SFML: setMouseCursorVisible 'needs to be called from the thread that created the window' on Windows (dep/SFML-3.0.2/include/SFML/Window/WindowBase.hpp:442-443).

**Where:** app/editor/Editor.cpp:136 calls into dep/imgui-sfml-3.0/imgui-SFML.cpp:494, which calls updateMouseCursor (:1211-1228), which calls WindowImplWin32::setMouseCursorVisible/setMouseCursor (dep/SFML-3.0.2/src/SFML/Window/Win32/WindowImplWin32.cpp:412-431) on the render thread. Meanwhile the main thread's WM_SETCURSOR handler reads the same m_cursorVisible and m_lastCursor (WindowImplWin32.cpp:742-747).

**Consequence:** A data race on the cursor fields. SetCursor is called from a thread that does not own the window, so ImGui's cursor-shape changes may not take effect (the last point is inferred).

### Item 3

**Rule:** A window or render target must not be mutated on one thread while another thread draws to it.

**Where:** core/Engine.cpp:90 calls window.setSize on the main thread. waitEvent's filterEvent then calls RenderWindow::onResize, which calls setView(getView()) (dep/SFML-3.0.2/src/SFML/Window/WindowBase.cpp:371-380; dep/SFML-3.0.2/src/SFML/Graphics/RenderWindow.cpp:122-125). Both run while the render thread clears, draws and displays at core/Engine.cpp:203-209.

**Consequence:** A data race on the view and viewport state: frames can be drawn with a torn view (undefined behaviour).

### Item 4

**Rule:** The EnTT registry is not thread-safe; never iterate a set while another thread modifies it (dep/entt-3.16.0/docs/md/entity.md:2335-2341).

**Where:** core/Engine.cpp:181-183 runs Sandbox::update on the physics thread, which destroys entities at app/scene/Sandbox.cpp:123-135. core/Engine.cpp:208 runs render unlocked on the render thread; it iterates Sprite views and moves sprites through checkBoundary at app/scene/Sandbox.cpp:145-157.

**Consequence:** Undefined behaviour and crashes during Play: iterators are invalidated and the render thread reads destroyed components.

### Item 5

**Rule:** EnTT structural changes must be exclusive of every other access to the registry.

**Where:** app/editor/Editor.cpp:295, 324 and 531 call registry.destroy on the render thread, while the physics thread runs update (core/Engine.cpp:181-183).

**Consequence:** Undefined behaviour. The quadtree keeps stale entity ids.

### Item 6

**Rule:** C++ memory model: an unsynchronized non-atomic write and read on two threads is a data race and undefined behaviour.

**Where:** The ms deltaTime float is written at app/editor/Editor.cpp:333 on the render thread and read at core/Engine.cpp:179 and :184 on the physics thread. m_selectedSceneData is written at app/editor/Editor.cpp:345 on the render thread and dereferenced at app/editor/Editor.cpp:129 on the physics thread.

**Consequence:** Undefined behaviour. The tick size can change in the middle of the catch-up loop, and the physics thread can step a scene the editor has just switched away from.

### Item 7

**Rule:** An object must not be destroyed while another thread is using it.

**Where:** core/Engine.cpp:204 runs SceneManager::processChange unlocked on the render thread; it pops and destroys scenes at core/manager/SceneManager.cpp:44 and :58. At the same time the physics thread can be inside getActiveScene()->update() at core/Engine.cpp:182.

**Consequence:** Use-after-free whenever a scene is replaced or removed.

### Item 8

**Rule:** Windows timer guidance: call timeBeginPeriod once, early, not around every wait; Sleep is quantized to clock ticks (https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleep).

**Where:** core/Engine.cpp:145 setFramerateLimit leads to Window::display, which sleeps at dep/SFML-3.0.2/src/SFML/Window/Window.cpp:203-205. sleepImpl then calls timeBeginPeriod, Sleep(truncated whole ms) and timeEndPeriod on every frame (dep/SFML-3.0.2/src/SFML/System/Win32/SleepImpl.cpp:47-54).

**Consequence:** The timer resolution is toggled every frame, which costs power and scheduler work. Pacing is quantized to whole milliseconds: the 6.25 ms target at 160 FPS becomes a sleep of 5 or 6 ms (computed).

### Item 9

**Rule:** A thread waiting for its next deadline should block, not spin. A spinning thread holds a core at full speed, which affects power, thermals and QoS.

**Where:** core/Engine.cpp:168-186. The physics loop never sleeps, waits or yields.

**Consequence:** One core stays pinned at 100%, even while the editor is paused. The audit measured 15.7 s of CPU in 15 s of wall time. On SMT the spin steals cycles from its sibling, and on laptops it costs battery and thermal headroom.

### Item 10

**Rule:** The window thread must keep pumping messages. Windows ghosts a top-level window that is unresponsive for several seconds (https://learn.microsoft.com/en-us/windows/win32/winmsg/about-messages-and-message-queues).

**Where:** core/Engine.cpp:70-73 joins the physics, render, audio and resource threads inside the Closed handler, and no messages are pumped while it waits.

**Consequence:** Minor. Shutdown waits for the update() call in progress (one GameOfLifeSim full-grid scan, for example) with no message processing. Windows only ghosts the window if that lasts several seconds, so this is not a crash.

### Item 11

**Rule:** SFML: a GL context must be active on the calling thread before any GL call (dep/SFML-3.0.2/include/SFML/Window/Window.hpp:317-322).

**Where:** core/Engine.cpp:198-199. A failed setActive(true) is only logged, and the render loop continues.

**Consequence:** All GL calls then run with no current context: they are silently ignored or undefined, and the engine keeps running.

### Item 12

**Rule:** render-optimization branch: the window must not be mutated concurrently with drawing.

**Where:** render-optimization:core/Engine.cpp:78-97. The Resized handler calls window.setSize at :92 without m_sceneMutex, while the render thread draws under a shared lock (:288-311) and displays outside it (:316).

**Consequence:** The same view race as on master; adding the shared_mutex did not cover it.

### Item 13

**Rule:** render-optimization branch: the SFML cursor calls must run on the window's thread.

**Where:** render-optimization:app/editor/Editor.cpp:170 calls ImGui::SFML::Update, and with it setMouseCursorVisible/setMouseCursor, on the render thread.

**Consequence:** The same cursor-state race as on master. ImGui itself no longer races, because main-thread ProcessEvent (core/Engine.cpp:113-114) holds the exclusive lock and render holds the shared lock.

### Item 14

**Rule:** render-optimization branch: writing under a reader (shared) lock is safe only while there is exactly one reader.

**Where:** render-optimization:app/editor/Editor.cpp:329, 358 and 586 destroy registry entities inside Editor::render, which runs under std::shared_lock at render-optimization:core/Engine.cpp:288.

**Consequence:** This is race-free today only because the render thread is the only shared holder. Adding any second reader turns it into undefined behaviour.

### Item 15

**Rule:** A task pool must wake its workers for new work. TaskScheduler workers park on a predicate that ignores the immediate queue (core/util/TaskScheduler.cpp:133, 185).

**Where:** render-optimization:core/system/WayPointSystem.cpp:127 calls parallel_for. TaskGroup::wait (render-optimization:core/util/TaskGroup.hpp:42-46) then runs on the physics thread while it holds the exclusive scene lock (render-optimization:core/Engine.cpp:215-227).

**Consequence:** Once the pool has been idle for more than 50 ms (for example, the editor paused before Play), parallel_for can wait forever while holding the exclusive lock. The render and main threads then block on that lock, and the whole app freezes.

### Item 16

**Rule:** Avoid oversubscription. Box2D's guidance is that efficiency cores and SMT give little benefit.

**Where:** render-optimization:core/Engine.cpp:169-177 sets job threads to hardware_concurrency()-2, which is 14 on the 16-logical-processor 7700X. The physics, render and main threads and two EventBus workers (core/ApplicationContext.hpp:35 on master) run alongside them.

**Consequence:** During parallel_for, more threads are runnable than there are logical processors, and SMT siblings contend. The physics thread sits idle in wait() instead of computing a share of the work.

## Render Optimization Branch

Branch render-optimization (5cc4cd76a) adds three threading pieces.

1. TripleBufferedRenderCommands2D (render-optimization:core/util/RenderCommands2D.hpp:59-91).
- Data: three RenderCommands2D buffers. Each is a vector of plain SpriteDrawCmd and RectDrawCmd values, and each SpriteDrawCmd holds a raw const sf::Texture*.
- Indices: an int m_writeIndex that only the producer touches, plus two atomics: m_readyIndex (the last published buffer) and m_readIndex (the buffer the consumer took).
- beginWrite() acquire-loads m_readIndex and skips that buffer.
- publish() release-stores m_readyIndex = m_writeIndex, then advances m_writeIndex.
- acquireRead() acquire-loads m_readyIndex, then release-stores that value into m_readIndex.
- It has two users:
  - Engine::m_renderCmds2D: written on the physics thread (render-optimization:core/Engine.cpp:232-244) and read on the render thread (:295).
  - EditorSceneAdapter::m_sceneViewCmds: written from Editor::update (render-optimization:app/editor/Editor.cpp:139) and drawn in Editor::render (:500).

2. One std::shared_mutex m_sceneMutex.
- The physics thread holds it exclusively for the whole tick: processChange, processInput, update and building the command buffer (core/Engine.cpp:215-255).
- The render thread holds it shared for the whole draw, including the entire ImGui editor frame (:288-311).
- display() runs outside the lock (:316).
- Main-thread event dispatch takes it exclusively (:99-115). The Resized handler's setSize (:78-97) does not take it.

3. TaskGroup and parallel_for (render-optimization:core/util/TaskGroup.hpp:19-79).
- Built on the unchanged TaskScheduler; `git diff --stat master...render-optimization` shows no changes to it.
- Used by WayPointSystem (render-optimization:core/system/WayPointSystem.cpp:100-191) with 14 job threads (core/Engine.cpp:169-177).

Correctness of the triple buffer.
- The memory orders are right for both purposes. publish's release store pairs with acquireRead's acquire load, so the consumer sees the buffer contents. The consumer's release store of m_readIndex pairs with the producer's acquire load, so a buffer is reused only after the consumer is done with it.
- The index protocol is not correct. acquireRead does a load and then a separate store, and during that gap the producer still sees the old m_readIndex. If the producer publishes twice inside the gap (two ticks, 16.7 ms at 120 Hz, which a preempted render thread can easily allow), beginWrite can choose the very buffer the consumer just took. The consumer's read then overlaps vector clear() and push_back. That is a torn frame, or a read of freed memory if the vector reallocates.
- The second if in beginWrite can never fire.
- There is no fresh flag, so the consumer cannot tell a new frame from a repeated one.
- Only the latest snapshot exists, so interpolation between the previous and current tick is impossible.
- Dropping frames (latest wins) is correct for drawing but wrong for anything that must be seen every tick.
- Commands carry raw texture pointers. An unload or scene swap would leave dangling pointers in the unread buffers; this is latent, because nothing unloads at runtime today.
- Nothing blocks, so shutdown has no hazard.
- On the branch, every producer and consumer call runs under the shared_mutex, exclusive against shared, so the race window is never reached.
- For the same reason the buffer buys nothing. Physics and render are serialized except while the render thread sits in display() (the vsync or frame-limit sleep). Frame latency and CPU utilization are therefore no better than a single thread with a lock.

Correctness of TaskGroup.
- (a) submit() moves fn into the task lambda before calling schedule(). The fallback path then calls the moved-from std::function (TaskGroup.hpp:29-38), which is empty on MSVC and libstdc++, so it throws std::bad_function_call. m_pending never reaches 0, and wait() hangs or the process terminates. The comment says the fallback handles a full queue, but schedule() blocks when the queue is full (core/util/TaskScheduler.cpp:25-39); it returns false only after shutdown.
- (b) completeOne() decrements m_pending and only then locks m_mtx and calls notify_all (TaskGroup.hpp:49-56). The waiter can observe 0 in that window, return, and destroy the stack-allocated TaskGroup (in parallel_for) before the notifier locks. That is a use-after-free.
- (c) It inherits TaskScheduler's lost wakeup (core/util/TaskScheduler.cpp:133, 185). After 50 ms of idleness, parked workers never pick up immediate tasks, so wait() can block forever while the physics thread holds the exclusive scene lock. The whole app freezes.
- (d) The caller blocks idle in wait() instead of running chunks itself. A nested parallel_for can deadlock the pool.
- (e) An exception thrown in a task reaches std::terminate (core/util/TaskScheduler.cpp:135).
- (f) 14 workers plus about 5 engine threads exceed the 16 logical processors.

What a new design can reuse.
- The plain-value draw-command structs, and the idea of a render list built from the World that the renderer draws without touching the registry.
- WayPointSystem's shape: gather inputs serially, compute in parallel into an output array, then scatter serially on the owning thread. Structural changes and emplace_or_replace stay on one thread, which is the correct fork-join pattern for EnTT.
- The submit / wait / parallel_for(count, grain, fn) API shape.

What it should replace.
- The index protocol. Use either one atomic middle slot swapped with exchange() that carries a fresh bit, or a ring of N tick-stamped snapshots so the renderer can interpolate.
- Raw texture pointers. Use handles that are retired only after the frames in flight.
- The shared_mutex. A renderer that reads only snapshots needs no lock on the World.
- TaskGroup and TaskScheduler. Use std::latch, or an atomic counter whose final decrement is the last access to the group. The caller should run work itself, exceptions should be captured, and workers should be sized to P-cores.

## Key Facts

- SDL3 + SDL_GPU + ImGui pin one thread, SDL's main thread: on Windows and Linux, the thread that called SDL_Init(SDL_INIT_VIDEO). Pinned to it are: creating and querying windows; pumping events (the Win32 message queue belongs to the window's creating thread); mouse state; SDL_Renderer; claiming, acquiring and waiting on the swapchain; the command buffer that presents; and the editor's ImGui frame. ImGui_ImplSDL3_NewFrame calls main-thread SDL functions, and multi-viewport windows are created and presented inside the UI frame.
- These can leave that thread: World/EnTT/Box2D stepping; offscreen GPU recording in command buffers acquired on another thread (one buffer per thread, submitted from that thread, ordered by the engine); GPU uploads with fences (cross-thread resource creation is likely but undocumented); fork-join workers; miniaudio's own device thread; asset decoding.
- The classic 'render thread owns the window' split used by today's SFML code is not available under SDL_GPU. The swapchain may only be touched from the window's thread, and the swapchain texture only by the command buffer that acquired it. A render thread can therefore only pre-record offscreen passes. The composite, ImGui and present stay on the platform thread. SDL performs no runtime check, so misuse is silent undefined behaviour.
- Blocking on the platform thread: acquire 'may block on vblank' under VSYNC, which is up to 16.7 ms at 60 Hz, or two 120 Hz ticks (computed). On Vulkan, SDL 3.4.14's non-blocking acquire uses an infinite timeout (issue 16548), and FIFO present blocks on Wayland and on NVIDIA under Windows with no threaded present yet (issue 11293). On Windows, the modal drag/resize loop blocks SDL_PollEvent for the whole drag; SDL iterates only main callbacks, or sends live-resize EXPOSED events, from a USER_TIMER_MINIMUM timer. A 120 Hz sim that shares that thread stalls with it. Only a separate tick thread, or SDL main callbacks plus MAILBOX, keeps the rate independent.
- Frames in flight default to 2 (range 1-3), and changing the value stalls and flushes the queue. VSYNC with SDR is the only present mode guaranteed to be available.
- Input: SDL_GetKeyboardState can be called from any thread but is unsynchronized; mouse state is main-thread only; gamepad getters are thread-safe. On Windows, SDL event timestamps come from millisecond message ticks. The sim thread should consume InputFrames latched at pump time and passed by value.
- Dear ImGui (vendored as 1.92.5, reporting 1.92.6 WIP) has a global, non-thread-safe context. ImDrawData is valid only until the next NewFrame, and its texture requests are live UI-thread state that NewFrame mutates and deletes. Replaying ImGui on another thread needs imgui_club's imgui_threaded_rendering v0.20 (ImDrawDataSnapshot::SnapUsingSwap plus a texture queue), which is not vendored; the alternative is lockstep, which serializes the threads. imgui_impl_sdlgpu3 requires PrepareDrawData before the render pass, submits its own command buffer for texture uploads on the calling thread, and calls SDL_WaitForGPUIdle when its buffers grow.
- EnTT 3.16 rules for threads:
  - The registry is not thread-safe.
  - A non-const view<T>() or storage<T>() can create a pool lazily, which is a structural change; pre-create every storage.
  - A const registry is fully thread-safe.
  - An owning group reorders its owned pools when any type it covers is added or removed, which defeats the 'disjoint components' rule (core/system/WayPointSystem.cpp:14).
  - Signals fire on the thread doing the mutation.
  - Registry and storage cannot be copied, so snapshots are explicit field copies.
  - The uint32 entity version has 12 bits and wraps after 4095 reuses, so match snapshots by the full id and use the Guid for long-lived references.
  - Define ENTT_USE_ATOMIC.
  - The organizer yields useful read-only/read-write graphs only when systems take views rather than registry&.
- Windows pacing: SDL_DelayNS already uses a per-thread high-resolution waitable timer (Windows 10 1803+) with no timeBeginPeriod. SDL_DelayPrecise spins about 1 ms or less per wait, at most about 12% of one core at 120 Hz (computed). timeBeginPeriod has been per-process since version 2004, and Windows 11 may ignore it for occluded or minimized processes. QoS lowers unfocused windows to Medium and hidden ones to Low (E-cores and low clocks on battery), unless the time-critical threads are tagged HighQoS with SetThreadInformation(ThreadPowerThrottling, ControlMask=EXECUTION_SPEED, StateMask=0).
- Linux pacing: absolute clock_nanosleep with high-resolution timers is typically microsecond-accurate. Timer slack is 50 us from init and inherited, and is not applied under real-time policies. The EEVDF scheduler transition began in kernel 6.6.
- Box2D v3: one stepping thread with internal data-parallel workers. The world must not be touched during b2World_Step, and queries are thread-safe outside it. Results are deterministic across thread counts and, since 3.1, across platforms when built with no fast-math and -ffp-contract=off. v3.2 changed the task callbacks and can start its own threads, so pin a version and feed it from the engine pool. It runs best on P-cores sharing one L3; E-cores and SMT help little. Kinematic bodies do not collide with static or kinematic bodies, and the 3.2 character mover is experimental.
- miniaudio mixes on its own device thread. Never init, uninit, start or stop the device from its callback. The node-graph read path is lock-free, while attach and detach use spinlocks. ma_engine/ma_sound thread-safety is undocumented, so drive them from one owner thread.
- C++ on both targets (MSVC 14.51 in VS 18, and libstdc++ in GCC 12 or later): jthread/stop_token, latch, barrier, counting_semaphore and atomic wait/notify all exist. atomic<shared_ptr> is not lock-free on either, so use an atomic index into engine-owned buffers for snapshot handoff. hardware_destructive_interference_size is 64 on MSVC; GCC tunes it per -mtune and warns against using it in headers.
- Tracy costs about 2.25 ns per zone (manual benchmark). Use separate FrameMarkNamed sets for sim ticks and render frames, wrap shared mutexes with TracyLockable, and name every thread. spdlog async logging uses 8192 slots and one worker by default; overrun_oldest never blocks the caller, and spdlog::shutdown() must run before main returns on Windows.
- Today's code breaks these rules: the ImGui context is driven from two threads (app/editor/Editor.cpp:117 against :136/:177); ImGui-SFML's cursor calls run on the render thread against SFML's window-thread rule; setSize and onResize race with drawing (core/Engine.cpp:90 against :203-209); registry update races with render (core/Engine.cpp:181-183 against :208); the editor destroys entities on the render thread; deltaTime and m_selectedSceneData are unsynchronized; processChange destroys scenes unlocked; timeBeginPeriod is toggled every frame by setFramerateLimit; the physics thread busy-spins.
- On render-optimization, the triple buffer's load/store gap is a real race, but the shared_mutex fully serializes the producer and consumer, so physics and render barely overlap and the buffer adds no concurrency. TaskGroup's fallback calls a moved-from std::function, completeOne() can touch a destroyed group, and the lost wakeup in the underlying TaskScheduler can freeze the app while the physics thread holds the exclusive lock. Worth reusing: the plain-value draw commands, gather-compute-scatter in WayPointSystem, and the submit/wait/parallel_for API shape.
- None of these constraints measures the single-thread ceiling (5k moving colliders at 120-160 Hz plus 10k sprites on one core). They show where threads may and may not go. Whether a multithreaded design beats one paced loop for these targets is still a measurement question.

## Unknowns

- Whether creating, mapping and releasing SDL_GPU resources concurrently from several threads is supported. SDL documents no rule; the Vulkan and D3D12 backends take internal locks, which suggests it is intended but does not prove it.
- Cross-thread GPU ordering. SDL guarantees only that a later submission begins after an earlier one begins. It does not document whether a texture written in thread A's command buffer is complete and visible when thread B's later submission samples it. A fence is the safe assumption; SDL's barrier behaviour across submissions was not verified.
- Where the D3D12 backend blocks under VSYNC (in acquire or in submit/Present), and for how long with 2 frames in flight on the owner's 7700X. Not measured.
- The thread rules for SDL_CreateGPUDevice, SDL_SetGPUSwapchainParameters and SDL_SetGPUAllowedFramesInFlight. None of their pages has a thread-safety section.
- Whether Windows 11, which ignores timer-resolution requests from occluded or minimized processes, also degrades CREATE_WAITABLE_TIMER_HIGH_RESOLUTION waits, which SDL_DelayNS uses.
- The real jitter and CPU cost of SDL_DelayPrecise on the 7700X, and on Intel hybrid laptops running on battery. Not measured.
- Whether tagging a thread HighQoS keeps it on P-cores under Intel Thread Director while on battery. Microsoft's documentation says 'standard high performance', not P-core placement.
- Whether ma_engine and ma_sound calls are thread-safe. The miniaudio manual does not say.
- The real resolution of the Win32 message tick that SDL uses for event timestamps. The tick is in milliseconds, but whether it advances at GetTickCount granularity was not checked. This decides whether input can be assigned to a position within a tick.
- Whether imgui_club's imgui_threaded_rendering v0.20 works with the vendored 1.92.6 WIP and imgui_impl_sdlgpu3 (nulled draw_data->Textures, the cycling buffers). Not built or tested.
- Whether MSVC's default /fp:precise avoids FMA contraction as fully as -ffp-contract=off. This decides cross-compiler determinism for Box2D and first-party code.
- Which Wayland compositors implement fifo-v1 and commit-timing-v1. This decides whether FIFO present blocks when the window is occluded.
- Which Box2D version to pin. The task-callback API changed between 3.1 and 3.2, and the newest release after v3.2.0 was not checked.
- The single-thread ceiling at the target loads (5k moving colliders at 120-160 Hz plus 10k sprites), and the overhead of snapshot copies at those loads. Neither has been measured.
- The 7700X topology (a single CCD with one shared L3), assumed when applying Box2D's 'single L3 cache' advice. Taken from general AMD knowledge, not verified here.
- The visible effect of SetCursor called from a thread that does not own the window, in the legacy SFML code. Inferred, not tested.
