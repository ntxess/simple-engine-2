// Copyright 2026 The simple-engine-2 Authors.

#include "support/content_test_module.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <expected>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/hash.h"
#include "se_core/log.h"
#include "se_core/schedule.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_sim/content_bundle.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"

#ifndef SE_FIXTURE_DIR
#error "SE_FIXTURE_DIR must name tests/fixtures"
#endif

namespace se::testing {

namespace {

constexpr std::uint64_t kStampSeed = 0xC047E475ULL;
constexpr int kMaxIncludeDepth = 4;

// The state a World belongs to (registry context; not hashed).
struct ContentRole {
  std::string_view state;
};

std::vector<std::string_view> Lines(std::string_view text) {
  std::vector<std::string_view> lines;
  while (!text.empty()) {
    const std::size_t end = text.find('\n');
    std::string_view line = text.substr(0, end);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    lines.push_back(line);
    if (end == std::string_view::npos) break;
    text.remove_prefix(end + 1);
  }
  return lines;
}

// Adds the entities of |path| (and its includes) to |world|; folds the
// texts' hashes into |stamp|.
std::expected<void, Error> AddText(World& world, const ContentSource& content,
                                   std::string_view path, int depth,
                                   Hasher& stamp) SE_REQUIRES(g_sim_owner) {
  if (depth > kMaxIncludeDepth) {
    return Fail(ErrorCode::kInvalidArgument, "includes nest too deep");
  }
  auto text = content.Text(path);
  if (!text) return std::unexpected(text.error());
  stamp.AddU64(ContentTextHash(*text));
  std::int32_t line_number = 0;
  for (const std::string_view line : Lines(*text)) {
    ++line_number;
    std::istringstream in{std::string(line)};
    std::string word;
    if (!(in >> word) || word.starts_with('#')) continue;
    if (word == "include") {
      std::string included;
      in >> included;
      if (auto added = AddText(world, content, included, depth + 1, stamp);
          !added) {
        return added;
      }
      continue;
    }
    std::string name;
    float x = 0.0F;
    float y = 0.0F;
    float vx = 0.0F;
    float vy = 0.0F;
    if (word != "entity" || !(in >> name >> x >> y >> vx >> vy)) {
      return std::unexpected(Error{ErrorCode::kParse,
                                   "expected 'entity NAME X Y VX VY' or "
                                   "'include PATH'",
                                   std::string(path), line_number, 1});
    }
    Hasher id(kStampSeed);
    id.AddString(path);
    id.AddString(name);
    const entt::entity e = world.Create(Guid{0xC0C0, id.Finish()});
    world.Reg().emplace<Transform>(e, Vec2{x, y}, 0.0F, Vec2{1.0F, 1.0F},
                                   Tick{0});
    world.Reg().emplace<Drift>(e, Vec2{vx, vy});
    world.Reg().emplace<Name>(e, name);
  }
  return {};
}

class ContentState final : public GameState {
 public:
  ContentState(const StateEnv& env, std::string_view id) : env_(env), id_(id) {}

  void OnEnter() SE_REQUIRES(g_sim_owner) override {
    WorldDesc desc;
    desc.name = std::string(id_);
    desc.kind = env_.world_kind;
    desc.seed = env_.seed;
    desc.tick_rate = env_.tick_rate;
    world_ = std::make_unique<World>(*env_.registry, std::move(desc));
    const MutationGate gate = world_->OpenGate(GateReason::kLoad);
    world_->Reg().ctx().emplace<ContentRole>(ContentRole{id_});
    Hasher stamp(kStampSeed);
    if (env_.content == nullptr) {
      SE_LOG_ERROR("content_test: state '{}' has no ContentSource", id_);
    } else if (auto built = AddText(*world_, *env_.content,
                                    ContentStateText(id_), 0, stamp);
               !built) {
      SE_LOG_ERROR("content_test: state '{}': {} ({}:{})", id_,
                   built.error().message, built.error().path,
                   built.error().line);
    }
    const entt::entity e = world_->Create(Guid{0xC0C0, 0x57A3});
    world_->Reg().emplace<ContentStamp>(e, ContentStamp{stamp.Finish()});
    world_->FinalizeSpawns();
    world_->RecordHash();
    worlds_[0] = world_.get();
  }

  void OnExit() SE_REQUIRES(g_sim_owner) override {
    worlds_[0] = nullptr;
    world_.reset();
  }

  [[nodiscard]] std::span<World* const> Worlds() const override {
    return world_ ? std::span<World* const>(worlds_)
                  : std::span<World* const>();
  }

 private:
  StateEnv env_;
  std::string_view id_;
  std::unique_ptr<World> world_;
  std::array<World*, 1> worlds_{};
};

std::unique_ptr<GameState> MakeTitle(const StateEnv& env) {
  return std::make_unique<ContentState>(env, kContentTitle);
}
std::unique_ptr<GameState> MakePlay(const StateEnv& env) {
  return std::make_unique<ContentState>(env, kContentPlay);
}
std::unique_ptr<GameState> MakePause(const StateEnv& env) {
  return std::make_unique<ContentState>(env, kContentPause);
}

struct DriftSystem {
  static constexpr std::string_view kName = "Drift";
  void Run(World& world, const TickContext& /*context*/)
      SE_REQUIRES(g_sim_owner) {
    for (const auto [e, drift, t] :
         world.Reg().view<Drift, Transform>().each()) {
      t.pos = t.pos + drift.velocity;
    }
  }
};

struct FlowSystem {
  static constexpr std::string_view kName = "Flow";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner) {
    const auto* role = world.CReg().ctx().find<ContentRole>();
    if (role == nullptr) return;
    const InputFrame& in = context.input;
    const auto pressed = [&in](ActionId action) {
      return in.pressed[action] != 0;
    };
    if (role->state == kContentTitle) {
      if (pressed(kContentStart)) {
        RequestGameState(world, StackOp::kReplace, kContentPlay);
      } else if (pressed(kContentQuit)) {
        RequestGameState(world, StackOp::kPop);
      }
    } else if (role->state == kContentPlay) {
      if (pressed(kContentPauseKey)) {
        RequestGameState(world, StackOp::kPush, kContentPause);
      }
    } else if (role->state == kContentPause) {
      if (pressed(kContentPauseKey)) {
        RequestGameState(world, StackOp::kPop);
      } else if (pressed(kContentQuit)) {
        RequestGameState(world, StackOp::kPop);
        RequestGameState(world, StackOp::kReplace, kContentTitle);
      }
    }
  }
};

void RegisterSystems(Schedule& schedule) {
  schedule.Add<DriftSystem>(Phase::kMovement);
  schedule.Add<FlowSystem>(Phase::kGameRules);
}

void RegisterStates(GameStateFactory& factory) {
  factory.Register(kContentTitle, &MakeTitle);
  factory.Register(kContentPlay, &MakePlay);
  factory.Register(kContentPause, &MakePause);
}

constexpr std::array<ActionBinding, 3> kBindings = {{
    {"start", "Return"},
    {"pause", "Escape"},
    {"quit", "Q"},
}};

}  // namespace

const GameModule& ContentTestModule() {
  static const GameModule kModule = {
      .name = "content_test",
      .register_components = &RegisterContentTestComponents,
      .register_systems = &RegisterSystems,
      .register_states = &RegisterStates,
      .start_state = kContentPlay,
      .actions = {.bindings = kBindings},
      .game_start_state = kContentTitle,
  };
  return kModule;
}

std::string_view ContentStateText(std::string_view state_id) {
  if (state_id == kContentTitle) return "content/title.txt";
  if (state_id == kContentPause) return "content/pause.txt";
  return "content/play.txt";
}

std::vector<std::string> ContentTestDependencies(std::string_view /*path*/,
                                                 std::string_view text) {
  std::vector<std::string> includes;
  for (const std::string_view line : Lines(text)) {
    std::istringstream in{std::string(line)};
    std::string word;
    std::string path;
    if ((in >> word) && word == "include" && (in >> path)) {
      includes.push_back(std::move(path));
    }
  }
  return includes;
}

std::string ContentFixtureRoot() { return SE_FIXTURE_DIR; }

CachedContentSource ContentFixtureSource() {
  return CachedContentSource(
      [](std::string_view path) -> std::expected<std::string, Error> {
        const std::string full = ContentFixtureRoot() + "/" + std::string(path);
        std::ifstream in(full, std::ios::binary);
        if (!in) {
          return std::unexpected(
              Error{ErrorCode::kNotFound, "no such text", full, 0, 0});
        }
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
      });
}

InputFrame PressFrame(Tick tick, ActionId action) {
  InputFrame frame;
  frame.tick = tick;
  frame.pressed[action] = 1;
  return frame;
}

InputFrame ScriptedPresses::Latch(Tick tick, std::uint64_t /*window_end*/,
                                  bool /*last_tick_of_frame*/) {
  InputFrame frame;
  frame.tick = tick;
  for (const Press& press : presses_) {
    if (press.tick == tick) frame.pressed[press.action] = 1;
  }
  return frame;
}

}  // namespace se::testing
