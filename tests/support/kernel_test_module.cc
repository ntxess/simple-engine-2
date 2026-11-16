// Copyright 2026 The simple-engine-2 Authors.

#include "support/kernel_test_module.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <entt/core/type_info.hpp>
#include <entt/entity/registry.hpp>
#include <expected>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/core_components.h"
#include "se_core/detmath.h"
#include "se_core/guid.h"
#include "se_core/jobs.h"
#include "se_core/rng.h"
#include "se_core/schedule.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"
#include "se_sim/session_log.h"
#include "support/kernel_test_components.h"

namespace se::testing {
namespace {

// Draw streams for scene generation.
enum SceneStream : std::uint32_t {
  kCenterX = 1,
  kCenterY,
  kRadius,
  kSpeed,
  kPhase,
  kChildPhase,
  kInputSegment,
};

float Unit(std::uint64_t seed, Tick tick, const Guid& id,
           std::uint32_t stream) {
  return rng::Unit(rng::At(seed, tick, id, stream, 0));
}

entt::entity AddMover(World& world, const Guid& guid, const Mover& mover)
    SE_REQUIRES(g_sim_owner) {
  const entt::entity e = world.Create(guid);
  world.Reg().emplace<Mover>(e, mover);
  world.Reg().emplace<Transform>(e, mover.center, 0.0F, Vec2{1.0F, 1.0F},
                                 Tick{0});
  return e;
}

void BuildGameplay(World& world) SE_REQUIRES(g_sim_owner) {
  const std::uint64_t seed = world.Desc().seed;
  for (int i = 0; i < kMoverCount; ++i) {
    const Guid guid = MoverGuid(i);
    Mover mover;
    mover.center = {-200.0F + 400.0F * Unit(seed, 0, guid, kCenterX),
                    -150.0F + 300.0F * Unit(seed, 0, guid, kCenterY)};
    mover.radius = 20.0F + 100.0F * Unit(seed, 0, guid, kRadius);
    mover.angular_speed = -2.0F + 4.0F * Unit(seed, 0, guid, kSpeed);
    mover.phase = 6.2831853F * Unit(seed, 0, guid, kPhase);
    const entt::entity e = AddMover(world, guid, mover);
    if (i % 4 == 0) {
      world.Reg().emplace<Name>(e, "mover " + std::to_string(i));
    }
  }
  const entt::entity player = world.Create(kPlayerGuid);
  world.Reg().emplace<Player>(player);
  world.Reg().emplace<Transform>(player);
  world.Reg().emplace<Name>(player, "player");

  const entt::entity spawner = world.Create(kSpawnerGuid);
  world.Reg().emplace<Spawner>(spawner);
  world.Reg().emplace<Transform>(spawner, Vec2{50.0F, -25.0F}, 0.0F,
                                 Vec2{1.0F, 1.0F}, Tick{0});

  const entt::entity follower = world.Create(kFollowerGuid);
  world.Reg().emplace<Follow>(follower, EntityRef{MoverGuid(5)}, 12.0F);
  world.Reg().emplace<Transform>(follower);
}

void BuildGameOver(World& world) SE_REQUIRES(g_sim_owner) {
  const entt::entity banner = world.Create(kBannerGuid);
  world.Reg().emplace<Banner>(banner, "GAME OVER", Tick{0});
  world.Reg().emplace<Transform>(banner);
  Mover mover;
  mover.radius = 40.0F;
  mover.angular_speed = 1.5F;
  static_cast<void>(AddMover(world, MoverGuid(900), mover));
}

class KernelState final : public GameState {
 public:
  KernelState(const StateEnv& env, bool game_over)
      : env_(env), game_over_(game_over) {}

  void OnEnter() SE_REQUIRES(g_sim_owner) override {
    WorldDesc desc;
    desc.name = game_over_ ? "game_over" : "gameplay";
    desc.kind = env_.world_kind;
    desc.seed = env_.seed;
    desc.tick_rate = env_.tick_rate;
    world_ = std::make_unique<World>(*env_.registry, std::move(desc));
    // Loading counts as one mutation of a Play World.
    const MutationGate gate = world_->OpenGate(GateReason::kLoad);
    if (game_over_) {
      BuildGameOver(*world_);
    } else {
      BuildGameplay(*world_);
    }
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
  bool game_over_;
  std::unique_ptr<World> world_;
  std::array<World*, 1> worlds_{};
};

std::unique_ptr<GameState> MakeGameplay(const StateEnv& env) {
  return std::make_unique<KernelState>(env, false);
}
std::unique_ptr<GameState> MakeGameOver(const StateEnv& env) {
  return std::make_unique<KernelState>(env, true);
}

// Systems.

struct PlayerInputSystem {
  static constexpr std::string_view kName = "PlayerInput";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner) {
    const InputFrame& in = context.input;
    const float dx = static_cast<float>(in.Held(kActionRight)) -
                     static_cast<float>(in.Held(kActionLeft));
    const float dy = static_cast<float>(in.Held(kActionDown)) -
                     static_cast<float>(in.Held(kActionUp));
    const auto dt = static_cast<float>(context.clock.dt);
    const Vec2 direction{dx, dy};
    for (const auto [e, player, t] :
         world.Reg().view<Player, Transform>().each()) {
      t.pos = t.pos + direction * (player.speed * dt);
      if ((in.held & 0x1FU) != 0) ++player.inputs;
    }
  }
};

// The pool keeps the site's state (M14): each session's pool has its own.
const jobs::ForkSiteDecl& MoverSite() {
  static const std::array<entt::id_type, 1> kReads = {
      entt::type_hash<Mover>::value()};
  static const std::array<entt::id_type, 1> kWrites = {
      entt::type_hash<Transform>::value()};
  static const jobs::ForkSiteDecl kSite{
      .name = "kernel_test.Movers",
      .grain = 16,
      .reads = kReads,
      .writes = kWrites,
  };
  return kSite;
}

struct MoverSystem {
  static constexpr std::string_view kName = "Movers";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner) {
    jobs::ForkSite& site = context.jobs.Site(MoverSite());
    const auto view = world.Parallel<Transform, Mover>(site);
    const double time = context.clock.Time();
    context.jobs.ParallelFor(site, view.size(), [&](jobs::ChunkRange range) {
      view.ForChunk(range, [&](entt::entity e) {
        const Mover& m = view.Read<Mover>(e);
        const double angle = static_cast<double>(m.phase) +
                             static_cast<double>(m.angular_speed) * time;
        const dm::SinCosPair<double> sc = dm::SinCos(angle);
        Transform& t = view.Write<Transform>(e);
        t.pos = {m.center.x + m.radius * static_cast<float>(sc.cos),
                 m.center.y + m.radius * static_cast<float>(sc.sin)};
        t.rot = static_cast<float>(dm::Atan2(sc.sin, sc.cos));
      });
    });
  }
};

struct FollowSystem {
  static constexpr std::string_view kName = "Follow";
  void Run(World& world, const TickContext& /*context*/)
      SE_REQUIRES(g_sim_owner) {
    entt::registry& reg = world.Reg();
    for (const auto [e, follow, t] : reg.view<Follow, Transform>().each()) {
      const entt::entity target = follow.target.Resolve(world);
      if (target == entt::null) continue;
      if (const auto* goal = reg.try_get<Transform>(target)) {
        t.pos = goal->pos + Vec2{follow.distance, 0.0F};
      }
    }
  }
};

struct SpawnerSystem {
  static constexpr std::string_view kName = "Spawner";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner) {
    const Tick tick = context.clock.tick;
    entt::registry& reg = world.Reg();
    const std::size_t alive = reg.storage<Lifetime>().size();
    struct Pending {
      Guid parent;
      std::uint32_t index;
      Vec2 at;
      Tick expire;
    };
    std::vector<Pending> pending;
    for (const auto [e, spawner, t] :
         reg.view<Spawner, const Transform>().each()) {
      if (tick % spawner.every_ticks != 0 || alive >= spawner.max_alive) {
        continue;
      }
      pending.push_back({reg.get<IdComponent>(e).guid, spawner.spawned++, t.pos,
                         tick + spawner.child_ttl});
    }
    for (const Pending& p : pending) {
      const entt::entity child = world.Spawn(p.parent, p.index);
      const Guid guid = reg.get<IdComponent>(child).guid;
      Mover mover;
      mover.center = p.at;
      mover.radius = 10.0F + 5.0F * static_cast<float>(p.index % 8);
      mover.angular_speed = 3.0F;
      mover.phase =
          6.2831853F * Unit(world.Desc().seed, tick, guid, kChildPhase);
      reg.emplace<Mover>(child, mover);
      reg.emplace<Transform>(child, p.at, 0.0F, Vec2{1.0F, 1.0F}, Tick{0});
      reg.emplace<Lifetime>(child, p.expire);
    }
  }
};

struct GameOverRule {
  static constexpr std::string_view kName = "GameOverRule";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner) {
    if (context.clock.tick != kGameOverTick) return;
    if (world.CReg().storage<Player>()->empty()) return;
    RequestGameState(world, StackOp::kReplace, kGameOverState);
  }
};

struct BannerSystem {
  static constexpr std::string_view kName = "Banner";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner) {
    for (const auto [e, banner] : world.Reg().view<Banner>().each()) {
      if (banner.shown_since == 0) banner.shown_since = context.clock.tick;
    }
  }
};

struct LifetimeSystem {
  static constexpr std::string_view kName = "Lifetime";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner) {
    std::vector<entt::entity> expired;
    for (const auto [e, life] : world.CReg().view<const Lifetime>().each()) {
      if (life.expire_tick <= context.clock.tick) expired.push_back(e);
    }
    for (const entt::entity e : expired) world.RequestDestroy(e);
  }
};

void RegisterSystems(Schedule& schedule) {
  schedule.Add<PlayerInputSystem>(Phase::kInput);
  schedule.Add<MoverSystem>(Phase::kMovement);
  schedule.Add<FollowSystem>(Phase::kMovement);
  schedule.Add<SpawnerSystem>(Phase::kGameRules);
  schedule.Add<GameOverRule>(Phase::kGameRules);
  schedule.Add<BannerSystem>(Phase::kGameRules);
  schedule.Add<LifetimeSystem>(Phase::kCleanup);
}

void RegisterStates(GameStateFactory& factory) {
  factory.Register(kGameplayState, &MakeGameplay);
  factory.Register(kGameOverState, &MakeGameOver);
}

constexpr std::array<ActionBinding, 5> kBindings = {{
    {"up", "W"},
    {"down", "S"},
    {"left", "A"},
    {"right", "D"},
    {"fire", "Space"},
}};

}  // namespace

const GameModule& KernelTestModule() {
  static const GameModule kModule = {
      .name = "kernel_test",
      .register_components = &RegisterKernelTestComponents,
      .register_systems = &RegisterSystems,
      .register_states = &RegisterStates,
      .start_state = kGameplayState,
      .actions = {.bindings = kBindings},
  };
  return kModule;
}

std::vector<std::byte> SceneBytes(std::uint64_t seed) {
  const std::string text =
      "kernel_test:gameplay movers=" + std::to_string(kMoverCount) +
      " seed=" + std::to_string(seed);
  const auto bytes = std::as_bytes(std::span<const char>(text));
  return {bytes.begin(), bytes.end()};
}

InputFrame ScriptedInput::Latch(Tick tick, std::uint64_t /*window_end*/,
                                bool /*last_tick_of_frame*/) {
  return FrameFor(tick, seed_);
}

InputFrame ScriptedInput::FrameFor(Tick tick, std::uint64_t seed) {
  // 40-tick segments, each holding one of none/up/down/left/right; fire for
  // 3 ticks every 97.
  constexpr Tick kSegment = 40;
  auto direction = [seed](Tick segment) {
    return static_cast<int>(rng::At(seed, segment, Guid{}, kInputSegment, 0) %
                            5);
  };
  InputFrame frame;
  frame.tick = tick;
  const int now = direction(tick / kSegment);
  const int before = tick == 0 ? 0 : direction((tick - 1) / kSegment);
  if (now != 0) frame.held |= 1ULL << (now - 1);
  if (tick % 97 < 3) frame.held |= 1ULL << kActionFire;
  if (now != before) {
    if (now != 0) frame.pressed[static_cast<std::size_t>(now - 1)] = 1;
    if (before != 0) frame.released[static_cast<std::size_t>(before - 1)] = 1;
    frame.edges[0] =
        InputEdge{static_cast<ActionId>(now == 0 ? before - 1 : now - 1),
                  now != 0, tick * 8'333'333ULL, 0, EdgeSource::kReplay};
    frame.edge_count = 1;
  }
  frame.axes[0] = static_cast<float>(tick % 100) / 100.0F;
  return frame;
}

std::string EditsFixturePath() {
  return std::string(SE_FIXTURE_DIR) + "/kernel/edits.txt";
}

std::expected<std::vector<SessionEntry>, Error> LoadEdits(
    const ComponentRegistry& registry, const std::string& path) {
  std::ifstream in(path);
  if (!in) return Fail(ErrorCode::kIo, "cannot open " + path);
  std::vector<SessionEntry> entries;
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    ++number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    Tick tick = 0;
    std::string guid_text;
    std::string component;
    std::string json;
    fields >> tick >> guid_text >> component;
    std::getline(fields >> std::ws, json);
    const auto guid = ParseGuid(guid_text);
    if (!fields.eof() || !guid || json.empty()) {
      Error error{ErrorCode::kParse, "malformed edit", path, number, 0};
      return std::unexpected(std::move(error));
    }
    auto entry = MakeComponentEntry(registry, SessionOp::kSetComponent, tick,
                                    *guid, component, json);
    if (!entry) {
      Error error = entry.error();
      error.path = path;
      error.line = number;
      return std::unexpected(std::move(error));
    }
    entries.push_back(std::move(*entry));
  }
  return entries;
}

}  // namespace se::testing
