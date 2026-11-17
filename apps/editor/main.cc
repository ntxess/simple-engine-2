// Copyright 2026 The simple-engine-2 Authors.
//
// simple_editor (docs/milestone-specs.md M10 and "Integration (M10a + M10b
// + M12 + M14)"): the editor shell on SDL's main callbacks, editing
// rampage's scenes with rampage's GameModule, with the panels of
// se_editor_panels docked in it (Hierarchy, Inspector, Viewport, Log,
// Profiler, Schedule, Stress generator, Scene settings).
//
//   simple_editor [runtime options] [-- --scene FILE] [--editor-script FILE]
//                 [--viewports]
//
// Runtime options are AppConfig's (se_runtime/app_config.h); every test
// and manual run passes --audio null or none (the default plays on the
// real device). --scene opens a scene file relative to the content root
// (default assets/scenes/sandbox.scene.json; a prefab file opens as a
// prefab document, its kind by its "format"); --editor-script (test
// builds) runs a script of editor actions at frame boundaries
// (se_editor/editor_script.h); --viewports turns on ImGui's
// multi-viewports. The editor paces Standard with frame latency 2, keeps
// SDL's raw keyboard off (IME, hotkeys) and starts in Edit mode.

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_rampage/module.h"
#include "game_rampage/scene.h"
#include "game_rampage/view.h"
#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/world.h"
#include "se_editor/editor.h"
#include "se_editor/editor_game.h"
#include "se_editor/panel.h"
#include "se_editor_panels/panels.h"
#include "se_editor_panels/stat_name_command.h"
#include "se_runtime/app.h"
#include "se_runtime/app_config.h"
#include "se_runtime/integration.h"
#include "se_runtime/pacer_policy.h"
#include "se_runtime/quit.h"

// Last: SDL_main.h with SDL_MAIN_USE_CALLBACKS, which provides main().
#include "se_runtime/sdl_main.h"

namespace {

constexpr const char* kDefaultScene = "assets/scenes/sandbox.scene.json";

se::editor::EntityJson ToEntityJson(se::rampage::EntityComponentsJson entity) {
  se::editor::EntityJson out;
  out.guid = entity.guid;
  out.components.reserve(entity.components.size());
  for (auto& [name, json] : entity.components) {
    out.components.push_back(
        se::editor::ComponentJson{std::move(name), std::move(json)});
  }
  return out;
}

se::editor::EditorGame RampageForTheEditor() {
  se::editor::EditorGame game;
  game.module = &se::rampage::RampageModule();
  game.load_scene =
      [](se::World& world, std::string_view text, const std::string& asset_root,
         const std::function<void(std::string_view, std::string_view)>&
             on_prefab) -> std::expected<void, se::Error> {
    // The editor calls it on main while it owns the World (inline mode).
    se::AssertMainThread();
    const se::InlineOwnerScope owner(se::OnMainThread());
    se::rampage::SceneLoadOptions options;
    options.asset_root = asset_root;
    options.on_prefab = on_prefab;
    auto loaded = se::rampage::LoadScene(world, text, options);
    if (!loaded) return std::unexpected(std::move(loaded.error()));
    return {};
  };
  game.save_scene = [](const se::World& world, std::string_view name) {
    // Flattened (kFlatten): the Play-start bytes, faults, the content hash.
    return se::rampage::SaveScene(world, name);
  };
  // M11a's writer (kPrefabs), prefab documents, the baseline and the
  // rebase are stubs until its next commits: they are filled then
  // (save_scene_file, load_prefab, save_prefab, prefab_baseline,
  // rebase_instances). Saves write the flattened scene meanwhile.
  game.removed_with = [](std::string_view component) {
    std::vector<std::string> out;
    for (const std::string_view derived : se::rampage::RemovedWith(component)) {
      out.emplace_back(derived);
    }
    return out;
  };
  game.removable = [](std::string_view component) {
    return se::rampage::Removable(component);
  };
  game.set_play_game_scene = [](const std::string& scene_path,
                                const std::string& asset_root) {
    se::rampage::SceneRequest request;
    request.format = se::rampage::SceneRequest::Format::kScene;
    request.path = scene_path;
    request.asset_root = asset_root;
    se::rampage::SetSandboxScene(std::move(request));
  };
  game.camera = [](const se::World& world) {
    return se::rampage::SandboxCamera(world);
  };
  return game;
}

std::expected<se::GameSetup, se::Error> MakeEditor(
    const se::AppConfig& config) {
  se::editor::EditorOptions options;
  options.scene = kDefaultScene;
  for (std::size_t i = 0; i < config.game_args.size(); ++i) {
    const std::string& arg = config.game_args[i];
    const bool has_value = i + 1 < config.game_args.size();
    if (arg == "--scene" && has_value) {
      options.scene = config.game_args[++i];
    } else if (arg == "--editor-script" && has_value) {
      if (!se::kRuntimeTestHooks) {
        return se::Fail(se::ErrorCode::kInvalidArgument,
                        "simple_editor: --editor-script needs a build with "
                        "the test hooks");
      }
      options.script = config.game_args[++i];
    } else if (arg == "--viewports") {
      options.viewports = true;
    } else {
      return se::Fail(se::ErrorCode::kInvalidArgument,
                      "simple_editor: unknown option '" + arg +
                          "' (expected --scene FILE, --editor-script FILE, "
                          "--viewports)");
    }
  }
  // The script's stat-name verbs: the Stats drawer's command group.
  options.script_hooks.stat_name_edit = &se::editor::MakeStatNameEdit;
  options.script_hooks.has_stat_name = &se::editor::HasStatName;
  options.script_hooks.stress_press = &se::editor::MakeStressPress;
  auto editor = std::make_shared<se::editor::Editor>(RampageForTheEditor(),
                                                     std::move(options));
  // The panels, in their dock order. The Profiler's button asks the crash
  // handler for a dump of the process as it is: the flight recorder (the
  // breadcrumbs, the log rings, the InputFrame ring, the session header
  // and, in Play, the Play-start stream).
  se::editor::PanelOptions panel_options;
  panel_options.dump_flight_recorder = [] {
    se::integration::RequestFaultDump("editor: flight recorder dump");
  };
  for (std::unique_ptr<se::editor::Panel>& panel :
       se::editor::MakeDefaultPanels(std::move(panel_options))) {
    editor->AddPanel(std::move(panel));
  }
  se::GameSetup setup;
  setup.module = &se::rampage::RampageModule();
  setup.title = "simple_editor";
  setup.org = "simple-engine";
  setup.app = "simple_editor";
  setup.content_marker = kDefaultScene;
  setup.seed = 1;
  setup.tick_rate = 120;
  setup.clear_color = se::ClearColor{0.0F, 0.0F, 0.0F, 1.0F};
  setup.window_width = 1600;
  setup.window_height = 900;
  setup.texture_table = "config/texture.json";
  setup.settings_file = "config/config.json";
  setup.sound_table = "config/sound.json";
  setup.camera = [](const se::World& world) {
    return se::rampage::SandboxCamera(world);
  };
  // The editor's shell: it owns the session, starts in Edit mode, idles
  // after 1 s without input, paces Standard with frame latency 2 and
  // leaves SDL's raw keyboard off.
  setup.extension = editor;
  setup.push_start_state = false;
  setup.lifecycle.idle_allowed = true;
  setup.default_pacing = se::PacingRequest::kStandard;
  setup.frame_latency = 2;
  setup.raw_keyboard = false;
  setup.quit_providers.push_back(editor.get());
  se::editor::Editor* raw = editor.get();
  setup.recovery = [raw](se::QuitReason reason) {
    // The quit's recovery hook runs on main (QuitController).
    se::AssertMainThread();
    raw->WriteRecoveryFiles(reason);
  };
  return setup;
}

}  // namespace

SE_DEFINE_SDL_APP(&MakeEditor)
