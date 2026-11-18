# Cost-free end state (the target the design builds on)

Synthesis of the 2026-11-07 cost-free audit re-run: new targets grown beside the stabilised legacy tree. The multithreaded design amends this end state (see mt-design.md, Changes To Cost Free End State).

## Headline

With costs at zero the verdict flips, narrowly: full redesign (3-0, margins of 0.5-1 point), grown as new targets in this repository beside a stabilised, deterministic copy of today's engine

## Recommended Shape

Full redesign, in this repository: grow a new engine as new CMake targets, starting from a minimal working slice, beside today's tree, which is kept as a deterministic behaviour reference. Build it to the target both cost-free cases converged on, using the redesign's goal-fit choices and the keep side's verification discipline.

1. Reference bootstrap, done in place on the legacy tree only. These are the same first steps as the kernel-first plan:
- one paced main-thread loop replacing the four threads (core/Engine.cpp:116-122, 161-213);
- SimTime ticks for effects and cooldowns instead of steady_clock (core/system/EventSystem.cpp:95,100,135,144; core/component/UpdateEntityPolling.cpp:4,9,20-22);
- Effects held by value (core/component/StatusModEvent.hpp:26);
- seeded RNGs (app/editor/Editor.cpp:939-940; app/scene/GameOfLifeSim.cpp:29-30);
- input sampled once per tick and recorded (app/scene/Sandbox.cpp:111-114);
- imgui_stdlib (app/editor/EditorComponentVisitor.cpp:39-41), and the ImGui delta passed in seconds (Editor.cpp:136 against Engine.cpp:143);
- a per-tick semantic projection and hash covering positions, stats and liveness, keyed by spawn order.
Tag the result and build it behind SE_BUILD_LEGACY.

2. Decide the kernel semantics as written rules with tests. Apply each rule to the reference as a declared commit and re-baseline its replays, so the two trees are always compared with the same semantics:
- TEMPTIMED always reverts (EventSystem.cpp:138-139 against :159-172).
- The cooldown is per (source, receiver) pair and decided once per tick (UpdateEntityPolling.cpp:18-24, called from core/system/CollisionSystem.cpp:88). readyOnStart is honoured or removed (:8-11).
- Every overlap is found. The reference uses brute force in place of the quadtree (core/util/QuadTree.cpp:12,124).
- Seconds are used throughout.
- Zero-length path segments are guarded (core/system/WayPointSystem.cpp:42-43).
- Contacts are sorted before effects apply.

3. Write the target down to contract level before the first new commit:
- the World: one registry, a SimClock and context resources, with no side registry and no simulation state held in systems;
- a phased Schedule, declared by a statically linked GameModule;
- the ComponentRegistry (glaze serialize/deserialize, clone, diff, a migrate hook and an inspect hook), with Guid and EntityRef;
- the RenderList and the material API;
- InputFrame semantics: held state plus the edges since the last step, and a buffer window;
- Collider shapes with Body, Hitbox, Hurtbox and Trigger roles, plus a layer/mask;
- the editor command model;
- written non-goals and a first-consumer rule.

4. Grow the new targets. No legacy file links into any of them:
- se_core (std, EnTT, glaze, spdlog);
- se_platform (SDL3);
- se_render (a headless RenderList plus a thin GPU backend);
- se_runtime and game_rampage;
- rampage (no ImGui);
- se_editor (the only ImGui target);
- se_tests and se_bench.
Build them in this order:
a) A headless World with determinism and round-trip tests, plus benchmarks: 5k moving colliders at 120-160 Hz, and 10k sprites through the RenderList.
b) An SDL3 window and sprite batch running the ported Sandbox as a scene file with prefabs.
c) The editor shell, as the second consumer: document and play worlds, commands, Play-as-clone through the serializer, Step and Stop, picking, and the ported widgets. From the first commit, panels take a const World plus an EditorCommands interface.
d) Save/load, prefabs, scoped asset handles and hot reload.
e) rampage, loading the same files through a GameStateStack.
f) Rampage primitives as their consumers land: tilemaps, parallax, Aseprite tags (and slices once the first animated character exists), audio, gamepad and shader effects.
g) Linux parity on Vulkan and Wayland/X11, in CI.

5. At every gameplay step, run the same recorded inputs through the reference and the new tree, and require equal projections tick for tick. Compare golden images of plain sprite batches across the backend change.

6. Keep a completeness ledger. Every legacy editor operation and gameplay rule is ported, re-specified, or deleted with a recorded reason.
- Turn these into characterization tests and run them against both trees: the path-editing rules (app/editor/Editor.cpp:828-885), the canvas controls (:903-930), the profiler axis rule (app/editor/EditorSceneSystemVisitor.cpp:29), the self and same-team skips (CollisionSystem.cpp:78-87), and OVERTIME's first application one interval after contact (EventSystem.cpp:98).
- Decide the generator's distributions explicitly (Editor.cpp:941,948,950).
- Restore component removal as a real registry command (Editor.cpp:508-517; cd906a37b).

7. Render backend. SDL_GPU is the expected end-state backend for two reasons: Rampage's visual-effects and colorblind settings need shader passes (config/config.json:22,54), and its ImGui backend supports viewports (dep/imgui-1.92.5/backends/imgui_impl_sdlgpu3.cpp:8,653). Size its shader set by the first shader consumer. Keep SDL_Renderer behind the same interface until that consumer lands, and afterwards as a hardware fallback.

8. Retire the reference once the ledger is closed and every recorded scenario matches. Each ported file records its source path and SHA, so git blame -C -C reaches the rationale commits. Then purge Boost from history.

## End State

### Runtime and main loop

**Area:** Runtime and main loop

**Design:** One thread touches world state. App (se_runtime) owns, in construction order: Platform (the SDL3 window and GPU device), AssetRegistry, InputService, Audio, Renderer2D and the GameStateStack. se_editor adds the EditorLayer.
Each frame:
1. Drain SDL events. In the editor ImGui sees them first, and its capture flags plus viewport focus gate game input. Losing focus clears held input.
2. Apply queued GameStateStack commands in FIFO order.
3. Run the kept fixed-step accumulator in seconds, with a 0.25 s frame clamp and at most 8 steps. Each step latches one InputFrame and steps every running World once, advancing SimClock{tick, dt, time}.
4. Build RenderLists from const Worlds, interpolated by alpha = accumulator/dt.
5. Run the editor UI with the real frame delta in seconds.
6. Present, paced by the present mode or a precise sleep. Never a busy spin.
The sim tick defaults to 120 Hz and is configurable; rendering runs at 60-160 FPS. App::create returns std::expected<App, Error>, App::requestQuit quits programmatically, and teardown is RAII. The only other thread is miniaudio's device thread, which never reads world state.

**Versus Today:** Four threads start at core/Engine.cpp:116-122. m_mutex covers only update() (:181-183) and processInput() (:205-207); processChange (:204) and render (:208) run unlocked. The physics loop busy-spins (:168-186), and two threads are empty (:218-243). deltaTime is stored in milliseconds (:143) but handed to ImGui as seconds (app/editor/Editor.cpp:136) [runtime.thread-per-subsystem].

### World and state domain

**Area:** World and state domain

**Design:** One World per scene holds:
- an entt::registry;
- a SimClock;
- an entt::dispatcher;
- ctx resources: broadphase scratch, a DebugDraw buffer, a seeded RNG, the stat schema and an AssetScope.
It is the only container of simulation state. Active effects live on the receiver as a component, systems keep no world state, and there is no side registry. Cloning, saving, undoing or destroying a World therefore covers everything by construction. Entities are destroyed by request, and the requests are drained once per step.

**Versus Today:** Each scene owns two registries and declares its SystemManager before both, so the systems outlive the registries they reference (app/scene/Sandbox.hpp:46-48). StatusModEvent stores entity ids from the other registry plus a raw Effects* (core/component/StatusModEvent.hpp:23-26). That pointer is taken from &effect (core/system/CollisionSystem.cpp:104), inside a vector the inspector erases from (app/editor/EditorComponentVisitor.cpp:67-70). Receivers are re-validated on every use (core/system/EventSystem.cpp:22-24). CollisionSystem keeps a persistent quadtree and a pipeline-registry reference outside any registry (CollisionSystem.cpp:5-7) [scenes.scene-owned-registries-and-pipeline-reg].

### Scenes, content and game flow

**Area:** Scenes, content and game flow

**Design:** Scenes are versioned asset files. Each holds settings (such as bounds, camera and music) and entities, where an entity has a Guid, components, and prefab instances with overrides. Loading a scene yields a World.
Game flow is a GameStateStack of polymorphic GameStates from the game module: Title, Gameplay, Pause and GameOver. GameStates own Worlds and request transitions. The stack keeps the deferred pushdown semantics of core/manager/SceneManager.cpp:40-69: push pauses the top, replace destroys it, pop resumes the one below. It adds a FIFO command queue and drops replace-by-default.
Scene-specific logic attaches as named Behaviour components registered by the GameModule. The GameModule is a statically linked struct {registerComponents, registerSystems, registerStates, startState}, not a plugin system. The editor is never a scene and never sits on the stack.

**Versus Today:** IScene bundles content, rules, input, drawing, registry exposure and visitor dispatch into 10 pure virtuals, and update() and render() take no arguments (core/interface/IScene.hpp:13-22). Content is written in C++ (app/scene/Sandbox.cpp:52-92). The texture block is duplicated (Sandbox.cpp:25-49 and MainMenu.cpp:25-49). Core names app classes (core/interface/ISceneVisitor.hpp:5-17), and the editor casts to Sandbox (app/editor/Editor.cpp:627). addScene replaces by default (core/manager/SceneManager.hpp:14; SceneManager.cpp:56-58), so a hosted scene's Escape (Sandbox.cpp:100) would replace the editor root. This is latent, because the editor forwards events only to ImGui (Editor.cpp:115-118).

### Components and lifetime policy

**Area:** Components and lifetime policy

**Design:** Components are plain aggregates, each registered once, with no base class, no virtuals and no platform types. Engine Vec2, Rect and Color types live in se_core.
The component set:
- Guid and Name;
- Transform;
- SpriteRenderer {texture handle, atlas region, origin, tint, layer, z, flip};
- Animator;
- TilemapLayer and ParallaxLayer;
- Camera2D;
- Collider {a small vector of AABB shapes; role Body, Hitbox, Hurtbox or Trigger; layer; mask};
- KinematicBody;
- MoveIntent and ActionIntent;
- Team;
- Stats, with interned stat ids;
- EffectSource {effect definitions, cooldown in ticks};
- ActiveEffects on the receiver {effect by value, EntityRef source, start tick, next tick, applied};
- Path {points, loop, segment, t};
- a one-level Attachment;
- AudioEmitter;
- DestroyRequest.
Rules:
- No component points into another.
- Cross-entity links are EntityRef {Guid, cached entity}, validated on every use.
- Derived structures are rebuilt each tick, or kept current by hooks their system owns.

**Versus Today:** Sprite : sf::Sprite is the only transform, and it doubles as the collider for every system, the quadtree and the editor (core/component/Sprite.hpp:12; CollisionSystem.cpp:73). An allocator_type typedef forces EnTT's allocator path (Sprite.hpp:35,41-49). Components derive from IComponent for visitor dispatch. PlayerInput owns raw ICommand pointers (core/component/PlayerInput.hpp:20). Paths are a linked list of WayPoints [ecs.sfml-types-as-components].

### Systems, schedule and gameplay rules

**Area:** Systems, schedule and gameplay rules

**Design:** Systems are small structs with run(World&). Each is registered with a phase and a name, and timed into a ring buffer and a Tracy zone. One phased Schedule (Input, Intent, Movement, Physics/Collision, Effects, GameRules, Cleanup) is declared once by the game module and shared by the runtime, editor play sessions and tests.
Collision:
- a spatial hash over cached world AABBs, rebuilt every tick;
- an AABB narrow phase and a layer/mask matrix;
- contacts sorted before effects apply;
- kinematic bodies resolved against solids and tiles with swept AABBs.
Effect rules live in the game module and run on ticks:
- INSTANT applies once.
- OVERTIME applies every tickRate ticks, floor(duration/tickRate) times, the first time one interval after contact.
- TEMPTIMED applies once and always reverts, at start+duration or on early removal.
- The cooldown is per (source, receiver) pair and decided once per tick.
- Self and same-team contacts are skipped.
Paths keep speed bound to a stat, looping, and heading taken from velocity, with zero-length segments guarded.

**Versus Today:** Each scene fills its own SystemManager in init (app/scene/Sandbox.cpp:90-92).
The quadtree files entities by position but matches them by bounds and prunes by node, so it misses overlaps that straddle nodes (core/util/QuadTree.cpp:12,117-118,124).
TEMPTIMED's watchdog returns before the revert (core/system/EventSystem.cpp:138-139 against :159-172). Effects use steady_clock and the editor's Pause gates only update() (app/editor/Editor.cpp:126-132), so pausing past the 36 s watchdog (Sandbox.cpp:91) during the 5 s debuff (Sandbox.cpp:88) makes the debuff permanent.
OVERTIME re-bases on now() and accumulates jitter (EventSystem.cpp:98-101).
The cooldown is consumed per receiver (core/component/UpdateEntityPolling.cpp:18-24, called from CollisionSystem.cpp:88), and readyOnStart is ignored (:8-11).
Speeds are in px per ms, and path movement divides by zero-length segments (core/system/WayPointSystem.cpp:37-43).
Collision never resolves (CollisionSystem.cpp:94-112) [systems.eventsystem-wallclock-effect-model] [systems.quadtree-point-region-impl].

### Rendering

**Area:** Rendering

**Design:** se_render is an engine-owned batched 2D renderer in two halves.
- The RenderList builder is headless and testable. From a const World, with interpolation, it culls by camera; collects sprites, animators, tilemaps, parallax, text, UI and debug primitives; sorts by layer, z or y-sort, and material; and emits quad batches.
- The GPU backend is thin: one streamed vertex buffer per frame, and one draw per material run.
The expected end-state backend is SDL_GPU (D3D12 on Windows, Vulkan on Linux). Its shaders are a tiny HLSL set compiled offline to DXIL and SPIR-V: sprite, solid and lines, text, tint or flash, and a colorblind transform. The set is sized by its first shader consumer, and SDL_Renderer can sit behind the same interface as a fallback.
Cameras render to the swapchain (rampage) or to offscreen textures shown with ImGui::Image (editor), using a virtual resolution, integer scaling and letterboxing. Atlases are built at import, tilemaps are chunked static buffers, and parallax is a per-layer camera transform. Text uses baked stb_truetype atlases. A screen-space UI layer covers menus, the HUD and damage numbers.
Rendering never writes simulation state. Render stats report draws, batches, quads and upload bytes.

**Versus Today:** Scenes issue one draw per object into a render texture the editor injects (app/editor/EditorSceneAdapter.cpp:11-12). Game of Life builds one RectangleShape per live cell every frame (app/scene/GameOfLifeSim.cpp:151-163), about 145k draws at its 7% fill (:28). Sandbox::render clamps sprite positions (app/scene/Sandbox.cpp:155,202-217). The aspect ratio is enforced by resizing the OS window (core/Engine.cpp:76-96). SFML 3.0.2 draws through fixed-function GL [crosscut.no-render-layer-immediate-draws].

### Editor

**Area:** Editor

**Design:** se_editor is an application over se_runtime and game_rampage, and the only target that links ImGui. It uses a tagged docking release with imgui_impl_sdl3 and imgui_impl_sdlgpu3. The dynamic font atlas and gamepad navigation are on, multi-viewports are opt-in, and one DockSpace is seeded once with DockBuilder and persisted.
Documents and play:
- Opening a scene or prefab creates an EditDocument: a World that is never stepped, with its own undo stack, selection and dirty flag.
- Play serializes the document into a fresh play World, so every Play is a round trip through the serializer.
- Pause stops stepping, and Step runs exactly one tick.
- Stop discards the play World and must leave an identical document hash.
- 'Play game' runs the module's GameStateStack from its start state.
Every Edit-mode change is a command: set, add or remove a component (with before/after JSON); create or destroy an entity (stored serialized); dedicated tile and path commands. Drags coalesce into one command.
Panels are classes that receive a const World and an EditorCommands interface. A headless test fails on any Edit-mode mutation made outside commands. The panels:
- Hierarchy;
- Inspector: reflection-generated drawers plus ported custom drawers for effects, team, stats and the waypoint canvas;
- Viewport: pan and zoom, picking through the broadphase, a 2D gizmo and the ported overlays;
- Assets browser;
- Tilemap painter;
- Animation preview;
- Log, fed by an spdlog ring sink;
- Profiler;
- Stress generator;
- Scene settings.
Hot reload polls file timestamps. Save writes the same files rampage loads.

**Versus Today:** The editor is the root IScene (app/editor/Editor.hpp:31; app/main.cpp:35). It hosts three hard-coded scene adapters (app/editor/Editor.cpp:85-104) with a signal-fed shadow entity map (app/editor/EditorSceneAdapter.hpp:78-79,113), and calls getRegistry() 27 times.
Play and Forward are two atomics over the live registry (Editor.hpp:119-120; Editor.cpp:126-132,242-262).
Inspectors write live components (app/editor/EditorComponentVisitor.cpp:31-70) through a const_cast buffer that can overflow (:39-41).
Component 'remove' only hides the shadow entry (Editor.cpp:508-517), because 0a8dbc557 dropped the registry removal that cd906a37b had added.
The Animation, Resource and Save sections and the Assets panel are empty (Editor.cpp:350-363,469-477). The log panel re-reads the log file every frame (core/util/LogStream.cpp:19-42) [editor.editor-operates-on-live-state] [editor.editor-is-root-iscene-hosting-adapters].

### Serialization, reflection and config

**Area:** Serialization, reflection and config

**Design:** One ComponentRegistry lives in se_core, keyed by entt::type_hash. Each component has one entry:
- a stable name and a version;
- serialize and deserialize, generated by glaze from the aggregate's members;
- clone and a JSON diff;
- a migrate hook;
- an inspect hook: a generic drawer built from the same members plus hints such as range and units, which custom drawers can override.
Engine components register in se_core, and game components in the GameModule. Identity is a Guid component; EntityRef fields serialize as Guids and resolve after load.
Scenes, prefabs and saves are versioned JSON, with BEVE optional for saves and Play snapshots. A prefab instance stores the prefab's AssetId plus one RFC 7386 merge patch per component. Arrays are replaced whole, and v1 has no nested prefabs.
Every format carries a version field, and migration code is written when a format reaches its second version. EngineConfig and UserSettings are typed aggregates with defaults. Errors are std::expected<T, Error{code, message, path, line, column}>. Writes are atomic: a temp file, then a rename.

**Versus Today:** Config lives in DataStore<std::any> with an exact any_cast (core/util/DataStore.hpp:74). It is filled by a 327-line RapidJSON DOM walk that flattens keys to leaf names (core/serializer/JsonDataStoreSerializer.cpp:118-255, :124) and swallows parse errors (:28-36). save() writes an empty store (:51-53). The game save is three stubs (core/serializer/GameDataStoreSerializer.cpp:11-24). Component types are listed by hand in three places that disagree (app/editor/Editor.cpp:208-220; app/editor/EditorSceneAdapter.cpp:62-72; core/interface/IComponentVisitor.hpp), and all content is C++ [data.no-component-reflection-or-scene-serialization].

### Assets and content pipeline

**Area:** Assets and content pipeline

**Design:** The content root is resolved once: from --root, then SE_ROOT, then by walking up from the executable to project.json. A per-user directory (SDL_GetPrefPath) holds logs, saves, settings and imgui.ini.
AssetRegistry:
- An AssetId is a normalized relative path plus a type.
- Typed stores (Texture, Atlas, AnimationSet, Tileset, Font, Sound, Music, Prefab, Scene) keep today's load-if-absent and reload semantics behind Handle<T>{index, generation}.
- Lifetimes are scoped: each GameState and EditDocument owns an AssetScope, and a persistent scope holds shared UI assets.
- A stale handle fails validation instead of dangling.
The asset index is built by scanning the root, with per-asset import settings.
Importers:
- PNG, through stb_image.
- Aseprite JSON and sheet exports: tags become clips, and slices become per-frame collider shapes and pivots once the first animated character exists.
- TTF, baked at fixed sizes.
- WAV, OGG and FLAC, through miniaudio.
The content primitives are animated sprites, editor-painted tilemaps, parallax stacks and prefabs.

**Versus Today:** Paths come from five raw current_path() calls. ResourceManager decodes a file before checking whether its key already exists (core/manager/ResourceManager.hpp:64-66). unload can free textures that sprites still point at (:106-120). The AssumeNew strategy has no caller (:24,73): the only load() calls, at app/scene/Sandbox.cpp:35 and app/scene/MainMenu.cpp:35, pass Reuse. config/texture.json lists 3 assets, and each scene repeats the same load loop. The tilesets and parallax art in assets/fg_city and assets/bg_city have no engine support [data.resmgr-get-reference-lifetime].

### Input and audio

**Area:** Input and audio

**Design:** InputService is fed only by SDL events: keyboard, mouse, and SDL_Gamepad with the community mapping database. An action map (move axes, jump, the six numbered action keys, enter, back, exit) is read from user settings, with defaults from config/config.json:30-46.
Each sim step consumes one InputFrame: the held state plus the pressed and released edges accumulated since the previous step. A tap between steps is therefore never lost, and a configurable buffer window serves chained actions.
Losing focus clears held state. In the editor, ImGui capture and viewport focus gate game input. InputFrames can be recorded and replayed, which is what the determinism tests use. No system polls the keyboard.
Audio is a miniaudio engine with Music and Effects groups, their volumes taken from config/config.json:26-29. AudioEmitter components and audio events are drained after the step.

**Versus Today:** Sandbox polls held keys once per rendered frame (app/scene/Sandbox.cpp:111-114), and each poll moves the player 1 px (core/command/Movement.cpp:11), so speed depends on frame rate. Nothing reads the bindings in config/config.json:30-46. sf::Joystick exposes only raw indices. Audio is an empty thread and an unused cache (core/Engine.cpp:218-228) [runtime.no-input-abstraction].

### Module layering and build

**Area:** Module layering and build

**Design:** New top-level targets in this repository, with layering enforced by the link graph:
- se_core: std, EnTT, glaze and spdlog only;
- se_platform: SDL3;
- se_render: SDL_GPU and stb;
- se_runtime and game_rampage;
- rampage: an executable with no ImGui;
- se_editor: the only ImGui target;
- se_tests and se_bench: headless, never opening a window.
Build setup:
- CMake 3.28+ presets with Ninja: Windows MSVC (debug, release, ASan), clang-cl, and Linux GCC and Clang with ASan/UBSan.
- An se_warnings interface target: /W4 /permissive- /utf-8 /Zc:preprocessor on MSVC; -Wall -Wextra -Wpedantic -Wshadow elsewhere.
- PRIVATE linkage by default, and SYSTEM includes for vendored code.
- Build checks that fail on banned includes in se_core or on removed legacy symbols.
Dependencies are vendored, trimmed and pinned, with a VERSIONS file. After retirement the legacy tree remains in history, and Boost is then purged from it.

**Versus Today:** There is one executable, editor_app (app/CMakeLists.txt:8). core_engine links five Boost libraries PUBLIC for a single logger, and the four SFML modules PUBLIC for the backend (core/CMakeLists.txt:13-33). It also carries a toml++ include path nothing uses (:17). There are no presets and 55 first-party /W4 warnings, and clang rejects code that MSVC accepts (core/serializer/JsonDataStoreSerializer.cpp:301).

### Tests, determinism and CI

**Area:** Tests, determinism and CI

**Design:** Catch2 v3 runs under CTest. Tests:
- determinism: the same InputFrames give the same Guid-keyed world hash after N ticks;
- a serialization round trip for every registered component, and load-save-load equality for every scene fixture;
- prefab overrides;
- effect semantics: application counts, revert guarantees, per-pair cooldowns;
- the broadphase against brute force on random worlds;
- path following, including zero-length segments;
- render-list sorting and batching;
- undo followed by redo is the identity;
- Stop restores an identical hash;
- the editor and the runtime load a scene into identical Worlds;
- the characterization tests extracted from the legacy rules.
Benchmarks: 5k moving colliders at the sim tick, and 10k sprites through the RenderList.
GitHub Actions builds from a clean clone: Windows MSVC (Debug, Release, ASan) and Linux GCC and Clang (ASan/UBSan). It runs ctest, clang-format and clang-tidy (bugprone, concurrency, performance). Tracy runs on demand. Golden hashes and replays are kept per platform.

**Versus Today:** tests/CMakeLists.txt is 0 bytes, and there is no CI. Runs cannot be reproduced: effects and cooldowns use steady_clock, the RNGs seed from std::random_device (app/editor/Editor.cpp:939-940; app/scene/GameOfLifeSim.cpp:29-30), and movement happens once per rendered frame.

## Dependency Changes

### SFML 3.0.2 (window, input, graphics, audio)

**Dependency:** SFML 3.0.2 (window, input, graphics, audio)

**Why:** SDL3 is the platform under Dear ImGui's first-party backends, which are already vendored (dep/imgui-1.92.5/backends/imgui_impl_sdl3.cpp). SDL_Gamepad's mapping database suits an action game; sf::Joystick exposes only raw button and axis indices. SDL3 runs natively on Wayland as well as X11, while SFML's vendored Unix backend is X11/GLX only. SDL3 also brings HiDPI, the clipboard, native file dialogs and a per-user preferences path. Raw throughput does not decide this. Confidence: medium [deps.sfml-3-from-source].

**With Costs:** Keep 3.0.2, built from source. Turn SFML_BUILD_NETWORK off, put Audio behind an option, make the codec downloads work offline, and stop components inheriting SFML classes.

**Without Costs:** Replace with a pinned stable SDL3 release (the re-ruling names 3.4.x), vendored and built static. SFML never enters the new targets and leaves with the legacy reference.

### 2D renderer and GPU API

**Dependency:** 2D renderer and GPU API

**Why:** Rampage's visual-effects and colorblind settings (config/config.json:22,54) need per-pixel passes such as hit flash, tints and a colour transform. The editor needs offscreen viewport targets. SFML draws through fixed-function GL. Of the two SDL-native ImGui renderers, the SDL_Renderer backend lists multi-viewport support as missing and warns that its graphics API is limited (dep/imgui-1.92.5/backends/imgui_impl_sdlrenderer3.cpp:4-10,17-18). The SDL_GPU backend supports both viewports and dynamic textures (imgui_impl_sdlgpu3.cpp:5-8,653). The owner's unmerged batcher already keys materials on texture, shader and blend mode.

**With Costs:** A first-party batched render layer on SFML Graphics, behind render(target). It reuses the design of BatchRenderer2D from the unmerged render-optimization branch, without that branch's threading.

**Without Costs:** An engine-owned Renderer2D: a headless RenderList plus a thin SDL_GPU backend (D3D12 on Windows, Vulkan on Linux). Its tiny HLSL set is compiled offline and sized by its first shader consumer. SDL_Renderer may sit behind the same interface as a fallback. bgfx and sokol_gfx offer more backend breadth than a 2D scope needs.

### ImGui-SFML 3.0 (editor backend)

**Dependency:** ImGui-SFML 3.0 (editor backend)

**Why:** It targets ImGui 1.91.1 (dep/imgui-sfml-3.0/CMakeLists.txt:41). It uploads fonts through the legacy GetTexDataAsRGBA32 path (imgui-SFML.cpp:637), and it implements no platform-window or texture callbacks. That caps the editor at the old font protocol and a single OS window. First-party backends ship inside ImGui, so their versions cannot drift apart [deps.imgui-sfml-3-0-backend].

**With Costs:** Keep it, editor-only, deliberately paired with the re-vendored ImGui, and record exact commits.

**Without Costs:** Remove it. Use the first-party imgui_impl_sdl3 and imgui_impl_sdlgpu3, already vendored in dep/imgui-1.92.5/backends.

### Dear ImGui (docking branch)

**Dependency:** Dear ImGui (docking branch)

**Why:** The editor needs docking (app/editor/Editor.cpp:76), and the vendored folder misstates its version: dep/imgui-1.92.5/imgui.h:32 reads 1.92.6 WIP. imgui_stdlib removes the const_cast buffer overflow at app/editor/EditorComponentVisitor.cpp:39-41. With first-party backends the editor gains the 1.92 dynamic font atlas, gamepad navigation and IME. Multi-viewports stay opt-in because the SDL3 platform backend notes problems on Windows (imgui_impl_sdl3.cpp:11-14).

**With Costs:** Upgrade to an exact tagged docking release, in a folder named after the tag, with imgui_stdlib and a version note.

**Without Costs:** The same upgrade, with the dynamic font atlas and gamepad navigation enabled and multi-viewports opt-in.

### RapidJSON 1.1.0 and DataStore<std::any>

**Dependency:** RapidJSON 1.1.0 and DataStore<std::any>

**Why:** With aggregate reflection, each component's field list exists once and drives JSON, the generic inspector and diffs. That removes the DOM walk into DataStore<any> (core/serializer/JsonDataStoreSerializer.cpp:118-255), including its leaf flattening (:124), the exact any_cast (core/util/DataStore.hpp:74) and the swallowed parse errors (:28-36). It also replaces three hand-kept type lists that disagree. RapidJSON's last tag dates from 2016. The risks are compile weight, MSVC's /Zc:preprocessor requirement and reflection that only works on aggregates; all are confined to the serialization translation units. Confidence: medium [deps.rapidjson-1-1-0].

**With Costs:** Keep RapidJSON and fix the serializer in place: check HasParseError and IsObject, restore save(path, data), stop opening files with fstream::app, and fix the clang error at :301.

**Without Costs:** Replace with glaze, pinned, used inside se_core's ComponentRegistry and for typed config. BEVE is optional for saves and Play snapshots. The fallback is nlohmann/json behind the same registry interface.

### Boost 1.90.0 (Boost.Log)

**Dependency:** Boost 1.90.0 (Boost.Log)

**Why:** Only core/util/Logger.hpp and Logger.cpp use Boost, yet five Boost libraries are linked PUBLIC (core/CMakeLists.txt:24-28). The tree carries about 1.3 GiB of tracked blobs, with paths long enough to break a default Git for Windows clone. With the call-site rewrite free, spdlog's ring or callback sink also replaces the editor's per-frame file tail (core/util/LogStream.cpp:19-42). Format-checked macros with compile-time level stripping replace the for-loop gate (core/util/Logger.hpp:20-38) [deps.boost-log-for-logging].

**With Costs:** Remove it. Use a std-only std::format logger behind the unchanged LOG_X(Logger::get()) macros, with spdlog as the alternative.

**Without Costs:** Remove it. Use spdlog, built with SPDLOG_USE_STD_FORMAT and SPDLOG_ACTIVE_LEVEL, behind format-style LOG_X macros. Purge Boost from history once the legacy reference no longer needs it.

### toml++ 3.4.0

**Dependency:** toml++ 3.4.0

**Why:** Its only first-party reference is a PUBLIC include path (core/CMakeLists.txt:17), and the .toml twin configs are never loaded. One config format, JSON through the reflection serializer, is enough [deps.tomlplusplus-unused].

**With Costs:** Remove.

**Without Costs:** Remove.

### EnTT 3.16.0

**Dependency:** EnTT 3.16.0

**Why:** Today's use is narrow and idiomatic: views, one owning group (core/system/WayPointSystem.cpp:14) and signals. entt::type_hash keys the ComponentRegistry, and entt::dispatcher carries World events. flecs v4 was considered. Its prefabs and reflection would replace part of the registry, but its runtime, scheduling and entity-as-everything model would take over the engine's data model.

**With Costs:** Keep, pinned, optionally consumed through the EnTT::EnTT target.

**Without Costs:** Keep, pinned, consumed through the EnTT::EnTT target.

### Test framework

**Dependency:** Test framework

**Why:** Either path needs a test target, and tests/CMakeLists.txt is 0 bytes. Catch2 v3 is actively released, and its BENCHMARK measures the goal loads directly. Its generators fit the property tests: the broadphase against brute force, and serialization round trips.

**With Costs:** Add doctest (a single header) under CTest; GoogleTest via FetchContent is also allowed.

**Without Costs:** Add Catch2 v3, vendored, with catch_discover_tests under CTest.

### Profiler

**Dependency:** Profiler

**Why:** SystemProfiler times only ISystem::update (core/util/SystemProfiler.cpp:9-20), so render, editor and pacing costs are invisible at the target loads. Tracy works on Windows and Linux and costs nothing while no client is connected.

**With Costs:** Keep the in-editor SystemProfiler and fix it; nothing else.

**Without Costs:** Add Tracy in on-demand mode, beside the in-editor per-system panel, which moves to a fixed ring buffer.

### Audio

**Dependency:** Audio

**Why:** Audio today is an empty thread and an unused SoundBuffer cache (core/Engine.cpp:218-228). miniaudio provides a mixer with sound groups for the configured music and effects volumes (config/config.json:26-29), plus streaming and decoders. It runs on its own device thread, which never touches the world. SDL3_mixer is the alternative.

**With Costs:** Keep SFML Audio behind a build option until sound is needed.

**Without Costs:** Add miniaudio, with stb_vorbis for OGG. SFML Audio goes.

### Image decoding and text

**Dependency:** Image decoding and text

**Why:** Every texture in assets/ is PNG, and pixel-art text at fixed sizes needs a baked atlas, not a shaping engine. stb_truetype already ships inside ImGui as imstb_truetype.h. SDL3_image and SDL3_ttf are the upgrade path if new formats or complex scripts become goals.

**With Costs:** Use SFML's own loaders (sf::Texture::loadFromFile and sf::Font).

**Without Costs:** Add stb_image (plus stb_image_write for thumbnails) and stb_truetype atlases drawn through the batcher.

### Physics

**Dependency:** Physics

**Why:** Collision never resolves today (core/system/CollisionSystem.cpp:94-112). The jump key and the six numbered action keys (config/config.json:36,40-45) suggest an action game. In that genre characters are kinematic and hits are hitbox-against-hurtbox overlaps, which a rigid-body solver would fight. Confidence: low.

**With Costs:** None: a first-party Collider, a rebuilt broadphase and a motion pipeline.

**Without Costs:** None in core. Collision is first-party: a spatial-hash broadphase, layer/mask filtering, Body/Hitbox/Hurtbox/Trigger roles and swept-AABB kinematic bodies. Box2D v3 comes in only if dynamic rigid bodies, such as debris, appear.

### Component reflection registry (first-party)

**Dependency:** Component reflection registry (first-party)

**Why:** It is the foundation for editor saves, prefabs, Play snapshots, undo, save games and data-driven scenes. It also collapses the three hand-kept type lists (app/editor/Editor.cpp:208-220; app/editor/EditorSceneAdapter.cpp:62-72; core/interface/IComponentVisitor.hpp) [data.no-component-reflection-or-scene-serialization].

**With Costs:** A core registry holding the serialize and clone half, so game_app can load scenes. Scene save/load with Play/Stop is scheduled; undo, prefabs and picking are left optional.

**Without Costs:** One ComponentRegistry in se_core, keyed by entt::type_hash, with a name, a version, glaze serialization, clone, diff, a migrate hook and an inspect hook. Game components register through the GameModule.

### Content import

**Dependency:** Content import

**Why:** The environment art is authored in Aseprite (.aseprite files under assets/fg_city), and an integrated editor should paint its own tilemaps. No character animation exists yet, so slice import waits for the first animated character.

**With Costs:** Not added.

**Without Costs:** Parse Aseprite JSON and sheet exports with glaze: tags become clips, and slices become per-frame shapes and pivots. Tilemaps are painted in the editor. No Tiled or LDtk dependency.

### Build, vendoring and CI

**Dependency:** Build, vendoring and CI

**Why:** Offline, patchable, pinned builds suit a solo owner or a small team. A clean-clone CI run stops vendored files from going missing silently. The 55 first-party /W4 warnings and a clang-only error (core/serializer/JsonDataStoreSerializer.cpp:301) show the need for more than one compiler [deps.vendoring-full-trees-in-git] [build.no-ci-analysis-format].

**With Costs:** Keep vendoring and merge fix-gitignore. Add a clean-clone Windows MSVC job first, then Linux GCC/Clang with sanitizers, clang-format and clang-tidy. Decide on a history rewrite for Boost, and drop the Dockerfile's unpinned clones.

**Without Costs:** The same model, plus: CMake presets with Ninja; trimmed, pinned vendored trees with a VERSIONS file; GitHub Actions running MSVC with ASan and GCC/Clang with ASan/UBSan; clang-format and clang-tidy gates; link-graph and banned-include checks; and a Boost history purge after retirement.

## Kept From Todays Code

- The fixed-step accumulator (core/Engine.cpp:165-185). Kept as code, moved to the main thread, in seconds, with a 0.25 s frame clamp and an 8-step cap.
- The effect model. This covers the INSTANT/OVERTIME/TEMPTIMED dispatch and arithmetic (core/system/EventSystem.cpp:54-176) and EffectType. The Effects shape {targetStat, modificationVal, maxDuration, tickRate} becomes EffectDef, and EffectsList becomes EffectSource. All of it moves into the game module and is re-timed on sim ticks. TEMPTIMED now always reverts, and OVERTIME's first application one interval after contact (:98) is kept as a written rule.
- The named stat bag with transparent hashing and its has/getOr/ref API (core/component/EntityStatus.hpp:20-37; core/util/TransparentStringHash.hpp). It becomes Stats, with interned ids and load-time validation.
- Team and the contact filters. The self skip and the same-team skip (core/system/CollisionSystem.cpp:78-87) become contact rules with tests. The cooldown idea is kept, now decided once per tick per (source, receiver) pair.
- The deferred pushdown semantics of SceneManager (core/manager/SceneManager.cpp:40-69): push pauses the top, replace destroys it, pop resumes the one below. Kept as the GameStateStack, with a FIFO command queue, an empty-stack check and no replace-by-default.
- Ordered system execution with per-system timing (core/manager/SystemManager.cpp:3-18; core/util/SystemProfiler.cpp:9-20). It becomes the phased Schedule, timed into a fixed ring buffer.
- The resource-cache semantics, load-if-absent and reload (core/manager/ResourceManager.hpp:64-120). They move inside typed stores behind Handle<T>, and the key is checked before decoding. AssumeNew is not kept, because nothing passes it.
- The path-following semantics (core/system/WayPointSystem.cpp): speed bound to a configurable stat, looping, and heading taken from velocity. They run on a point vector, with zero-length segments guarded.
- The editor ImGui code, ported and rebound to the new model. A large share of these lines (about 40% by the keep and design-risk counts) touch Sprite, WayPoint, the registry or sf:: and are re-implemented on either path. The pieces:
  - the waypoint canvas (app/editor/Editor.cpp:677-931). Its path-editing rules (:828-885) and its controls (:903-930: left-click adds a point, right-drag pans, right-click opens a menu, the wheel zooms in 0.1 steps within 0.1-10) become command tests;
  - the effect-list, team and stat drawers (app/editor/EditorComponentVisitor.cpp), with imgui_stdlib replacing the const_cast at :39-41;
  - the log panel layout and severity colours (Editor.cpp:380-427), and the performance panel (:368-378);
  - the entity overlays (:538-620);
  - the letterbox math (:451-464, :632-656);
  - the Play/Pause/Forward toolbar (:242-262), which becomes Play/Pause/Step/Stop;
  - the entity generator (:933-984), kept as a stress tool. Its distributions (:941,948,950) are either kept or redefined explicitly.
- The per-system profiler graphs, with their axis rule max(avg*1.5, peak*1.1, 1.0) (app/editor/EditorSceneSystemVisitor.cpp:29).
- The Game of Life step (app/scene/GameOfLifeSim.cpp:53-84). It becomes a sample state and a CPU benchmark, drawn as one streamed texture.
- Data. Everything in assets/; the config values for resolution and frame rate (config/config.json:14-19), volumes (:26-29) and key bindings (:30-46); and the texture and font keys. The generator's enemy archetype (app/editor/Editor.cpp:966-980) and the Sandbox player setup (app/scene/Sandbox.cpp:52-92) become prefabs and a scene file.
- Design kept without its code:
  - the LOG_TRACE..LOG_FATAL facade (core/util/Logger.hpp:27-38), re-expressed as spdlog format macros;
  - the render-to-texture viewport shown through ImGui::Image;
  - C++23 with extensions off;
  - pinned, vendored dependencies;
  - EnTT 3.16.0 and Dear ImGui docking;
  - the material key (texture, shader, blend) and the sort modes of BatchRenderer2D, from the unmerged render-optimization branch.
- The documented fixes, kept as test specifications or recorded dispositions:
  - b4d591eb9 (RenderTexture orientation): moot under the new backend, covered by a viewport test;
  - da4dd23ac (self-collision);
  - cd906a37b (missing target stat): becomes schema validation at load, and its component removal is restored as a real registry command;
  - 73fcf86b6 (resource lifetime): moot with handles;
  - ca6f38f41 and c0d7336da (tick semantics);
  - the aspect behaviour at core/Engine.cpp:76-96: becomes a letterbox camera.
- The whole legacy tree, as the reference. It is stabilised, made deterministic, tagged and built behind SE_BUILD_LEGACY, and it receives each declared semantic change until the completeness ledger closes. Ported files record their source path and SHA, so git blame -C -C reaches the rationale commits: ca6f38f41, c0d7336da, 74f176050, 73fcf86b6, da4dd23ac and cd906a37b.
- Not kept on either path:
  - the four-thread Engine and its mutex;
  - ApplicationContext;
  - IScene, ISceneVisitor, IComponent, IComponentVisitor and ICommand;
  - EditorSceneAdapter and its shadow map;
  - Sprite : sf::Sprite, SceneViewRenderer, Hitbox and the Font wrapper;
  - the QuadTree and the pipeline registry;
  - UpdateEntityEvent and UpdateEntityPolling;
  - the WayPoint linked list;
  - DataStore<std::any> and the three serializers;
  - SyncPolicy, EventBus, TaskScheduler and BoundedQueue;
  - the log-file tail and the Boost.Log backend;
  - toml++ and the .toml twins.

## What Did Not Change

- The goals: a 2D engine with an integrated editor; thousands of active entities at 60-160 FPS; Windows first, Linux desired; years of maintenance by a solo owner or a small team; Rampage as the first game. Arguments that only work under other goals (3D, consoles, mobile, a simulation that is parallel from the start) stay set aside.
- The diagnosis. The design is wrong in the kernel (the thread-per-subsystem runtime, the IScene contract, the time model, Sprite as the only transform, the broadphase, and live-state editing) and broadly sound outside it. The kernel is rewritten on either path.
- Correctness and safety do not depend on the path. One paced main-thread loop removes every race (core/Engine.cpp:116-122,181-183,205-208). One lifetime policy removes every UB site: values instead of pointers across entities, EntityRef validated on every use, and one deferred destroy point [crosscut.entity-handle-and-reference-lifetime-policy].
- The threading model: no thread per subsystem, no engine mutex and no job system. The threads are replaced, not locked. Parallel work enters only as fork-join inside one system, and only after a measured miss [runtime.thread-per-subsystem].
- The performance ceiling is the same on both paths and still unmeasured: a grid or hash rebuilt each tick, plus batching. Benchmarks come first: 5k moving colliders at a 120-160 Hz tick, and 10k sprites.
- The designs judged sound survive either way: the fixed-step accumulator, the pushdown stack semantics, ordered systems with timing, the effect arithmetic, the stat bag, the cache semantics, the log facade, the profiler, the waypoint canvas and the inspector widgets.
- The first work: stabilise today's tree in place, with one loop, effects held by value, imgui_stdlib and SimTime.
- Several dependency decisions: EnTT 3.16.0 stays; Dear ImGui moves to an exact tagged docking release with imgui_stdlib; Boost and toml++ go; vendored, pinned sources stay; CI with sanitizers, clang-format and clang-tidy is added; tests are wired under CTest.
- Engine and game are separated: the team, effect and HP rules leave the engine library (core/system/CollisionSystem.cpp:53,63,83) for a game module [crosscut.gameplay-rules-inside-engine-library].
- No physics library in core: characters are kinematic, on first-party collision. Box2D v3 comes in only if dynamic rigid bodies appear.
- Determinism holds per build and per platform, so replay traces and golden hashes are kept per platform.

## Conditions That Would Flip

- The owner confirms that the integrated editor stays a live inspector over content written in C++: no scene or prefab files, no Play-as-clone, no undo. The choices about state domains and contract shapes, where inherited structure does its harm, then stop mattering, and keep's exact replay chain over the existing workflows wins. All three judges name this condition.
- An in-place plan writes the whole target down to contract level before migrating, and enforces a comparison gate. Every incumbent contract it keeps must beat a goal-derived alternative in a written comparison, nothing survives without a caller, and no state lives outside the World. An end-state audit then shows none of the shapes this debate surfaced: no second registry, no stateful systems, no IScene-shaped hooks, no AssumeNew. The two end states are then identical, and keep's extra verification makes it the better path. All three judges name this condition.
- Behaviour with real authority builds up before the work starts: tuned Rampage feel, frame data, saved scenes or shipped levels. Fidelity to the legacy code then carries weight, and keeping behaviour by default, together with the replay chain, dominates. All three judges name this condition.
- The reference fails. After the single loop, SimTime, seeded RNG and tick-sampled input are in place, two replays of the legacy tree still diverge on the semantic projection. Or declared semantic changes cannot be applied to it without importing new structure. Or side-by-side runs show that the fresh tree's behaviour inventory is materially incomplete. In each case the fresh path loses the check that makes its omissions visible. Named by the design-risk and architecture judges.
- The fresh build breaks its own rules against the second-system effect. A first-consumer audit finds abstractions that no Rampage or demo code uses: a plugin-style GameModule, a render graph, a scripting VM, nested prefabs, per-component migration machinery before any format has a second version, or a shader pipeline with no shader effect. Named by the architecture and design-risk judges.
- SDL_GPU proves unsuitable for the Windows-first target, and SDL_Renderer becomes the end-state backend. Possible causes are target machines that lack D3D12 or Vulkan, or a shader toolchain that cannot be kept working. That removes the largest goal-fit difference between the two written specs. Named by the capability judge.
- Release benchmarks taken after the grid and the batcher land show a structural miss at 5k moving colliders at 160 Hz, or at 10k sprites. That does not pick a side by itself. It reopens the data model on both paths, removes the premise that the two end states converge, and calls for a new comparison. Named by the capability judge.

## Caveats

- The margin is narrow on every lens: 1.0, 0.5 and 1.0 points on a 10-point scale. The panel is unanimous on direction, not on strength, and one of the three judges rated confidence low. Read 'medium' as the low end of medium.
- The verdict assumes the plain reading that the integrated editor authors and saves content: scene and prefab files, undo, Play-as-clone. The goals do not settle this. The code points that way but does not decide it: the Animation, Resource and Save sections and the Assets panel are empty (app/editor/Editor.cpp:350-363,469-477), and config/config.json:11,30-46 holds a save folder and key bindings that nothing reads. If the editor stays a live inspector over C++ content, the edge falls to near zero or reverses.
- This answers a counterfactual about the better end state and the better process. On its own it does not say to start a new tree while costs are real; see howToUseThisResult.
- Part of the anchoring evidence is behavioural. It comes from what the keep advocate and the neutral re-ruling specified when asked for a cost-free in-place end state, backed by code facts about the shapes they kept. It shows that in-place review tends to keep those shapes, not that an in-place team must.
- The redesign's own failure mode is real and only mitigated: the debate found three behaviours its port list missed. The plan depends on the legacy reference actually being made deterministic, and on keeping it current through every declared semantic change.
- 'Full redesign' here means a new architecture grown as new targets inside this repository. Today's tree stays as a reference, and its portable code and sound designs are ported. It does not mean a new repository, discarded history, or re-deriving the sound algorithms.
- Technical risk in the end state sits mainly in the new picks. SDL_GPU brings an offline HLSL-to-DXIL and SPIR-V toolchain, differences between D3D12 and Vulkan, no GPU in CI, and machines that may lack both APIs. glaze brings compile weight, MSVC's /Zc:preprocessor requirement, and reflection that only works on aggregates. Each has a named fallback: SDL_Renderer behind the same render interface, and nlohmann/json behind the same registry. The editor-facing contracts are new on both paths: undo granularity, prefab merge patches that replace arrays whole, and the document/play split.
- The single-thread ceiling is unmeasured on both paths: 5k moving colliders at a 120-160 Hz tick plus 10k sprites, all on one core. Floating-point results differ across MSVC, GCC and Clang, so determinism, replays and golden hashes hold per platform only.
- The SDL3 platform backend marks its multi-viewport support as partial on Windows (dep/imgui-1.92.5/backends/imgui_impl_sdl3.cpp:11-14). Viewports therefore stay opt-in, and the docked single-window layout is the default.
- Synthesizer checks against the repository confirmed the load-bearing claims:
  - app/scene/Sandbox.hpp:46-48;
  - core/component/StatusModEvent.hpp:23-27;
  - core/system/EventSystem.cpp:95-101,138-139,159-172;
  - core/component/UpdateEntityPolling.cpp:8-24;
  - core/util/QuadTree.cpp:12,117-124;
  - core/manager/ResourceManager.hpp:24,73, whose only callers pass Reuse;
  - app/editor/Editor.cpp:126-136,508-533,939-950;
  - 27 getRegistry() calls in Editor.cpp;
  - a 0-byte tests/CMakeLists.txt;
  - commit 0a8dbc557 deleting the getRegistry().remove<T>() call after cd906a37b had added registry removal.
  Three corrections, none of which changes the verdict:
  (a) The capability ruling's 'only imgui_impl_sdlgpu3 sets RendererHasViewports' overstates. The vendored opengl2, opengl3, vulkan and dx9-dx12 backends set it too. What holds is that the SDL_Renderer backend does not, so editor viewports need a real GPU API: SDL_GPU, or SDL3 with OpenGL or Vulkan.
  (b) config/config.json:40-45 are six numbered keys (key-1 to key-6) that nothing reads. Calling them attacks is the debate's inference about a brawler.
  (c) The defect where a hosted scene's Escape would replace the editor root is latent, because the editor forwards events only to ImGui (app/editor/Editor.cpp:115-118).
