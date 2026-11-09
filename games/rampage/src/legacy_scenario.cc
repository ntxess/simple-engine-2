// Copyright 2026 The simple-engine-2 Authors.

#include "game_rampage/legacy_scenario.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <expected>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/json.h"
#include "game_rampage/scene.h"
#include "game_rampage/systems.h"
#include "se_core/core_components.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/hash.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"

namespace se::rampage {
namespace {

constexpr std::uint64_t kLegacyGuidSeed = 0x1E6AC75CE4A210ULL;

std::unexpected<Error> Fail(std::string_view where, std::string_view what) {
  return std::unexpected(
      Error{ErrorCode::kParse, std::format("{}: {}", where, what), {}, 0, 0});
}

// The parsed scenario (ScenarioLoader.hpp's structs).
struct LegacyEffect {
  // nullopt: NULLTYPE, which completes without an effect.
  std::optional<EffectKind> kind = EffectKind::kInstant;
  std::string stat = "HP";
  float value = 0.0F;
  std::int64_t duration_ms = 0;
  std::int64_t interval_ms = 0;
};

struct LegacyPolling {
  std::int64_t cooldown_ms = 0;
  bool ready_on_start = true;
};

struct LegacyEntity {
  TeamId team = TeamId::kEnemy;
  std::string texture = "player";
  std::optional<Vec2> position;
  float rotation = 0.0F;
  std::vector<std::pair<std::string, float>> stats;
  std::vector<Vec2> path;
  bool repeat = true;
  bool moving = true;
  std::optional<LegacyPolling> polling;
  std::optional<std::vector<LegacyEffect>> effects;
};

struct LegacyPlayer {
  std::optional<Vec2> position;
  std::vector<std::pair<std::string, float>> stats;
  std::optional<LegacyPolling> polling;
  std::optional<std::vector<LegacyEffect>> effects;
};

struct LegacyScenario {
  std::string name;
  std::optional<std::int64_t> event_watchdog_ms;
  LegacyPlayer player;
  std::vector<LegacyEntity> entities;
};

class Reader {
 public:
  static std::expected<void, Error> Number(const json::Value& v,
                                           const std::string& where,
                                           float& out) {
    const auto d = v.AsDouble();
    if (!d) return Fail(where, "expected a number");
    out = static_cast<float>(*d);
    return {};
  }

  static std::expected<void, Error> Integer(const json::Value& v,
                                            const std::string& where,
                                            std::int64_t& out) {
    const auto i = v.AsInt64();
    if (!i) return Fail(where, "expected an integer");
    out = *i;
    return {};
  }

  static std::expected<void, Error> Boolean(const json::Value& v,
                                            const std::string& where,
                                            bool& out) {
    if (!v.IsBool()) return Fail(where, "expected true or false");
    out = v.AsBool();
    return {};
  }

  static std::expected<void, Error> String(const json::Value& v,
                                           const std::string& where,
                                           std::string& out) {
    if (!v.IsString()) return Fail(where, "expected a string");
    out = v.Text();
    return {};
  }

  static std::expected<void, Error> Point(const json::Value& v,
                                          const std::string& where, Vec2& out) {
    if (!v.IsArray() || v.Items().size() != 2 || !v.Items()[0].IsNumber() ||
        !v.Items()[1].IsNumber()) {
      return Fail(where, "expected [x, y]");
    }
    auto x = Number(v.Items()[0], where, out.x);
    if (!x) return x;
    return Number(v.Items()[1], where, out.y);
  }

  static std::expected<void, Error> StatList(
      const json::Value& v, const std::string& where,
      std::vector<std::pair<std::string, float>>& out) {
    if (!v.IsObject()) return Fail(where, "expected an object of stat values");
    for (const json::Member& m : v.Members()) {
      float value = 0.0F;
      auto n = Number(m.value, where + "." + m.key, value);
      if (!n) return n;
      out.emplace_back(m.key, value);
    }
    return {};
  }

  static std::expected<void, Error> Polling(const json::Value& v,
                                            const std::string& where,
                                            LegacyPolling& out) {
    if (!v.IsObject()) return Fail(where, "expected an object");
    for (const json::Member& m : v.Members()) {
      if (m.key == "cooldownMs") {
        if (auto r = Integer(m.value, where + ".cooldownMs", out.cooldown_ms);
            !r) {
          return r;
        }
      } else if (m.key == "readyOnStart") {
        if (auto r =
                Boolean(m.value, where + ".readyOnStart", out.ready_on_start);
            !r) {
          return r;
        }
      } else {
        return Fail(where, std::format("unknown key \"{}\"", m.key));
      }
    }
    return {};
  }

  static std::expected<void, Error> Effects(const json::Value& v,
                                            const std::string& where,
                                            std::vector<LegacyEffect>& out) {
    if (!v.IsArray()) return Fail(where, "expected an array of effects");
    for (std::size_t i = 0; i < v.Items().size(); ++i) {
      const std::string at = std::format("{}[{}]", where, i);
      const json::Value& e = v.Items()[i];
      if (!e.IsObject()) return Fail(at, "expected an object");
      LegacyEffect effect;
      for (const json::Member& m : e.Members()) {
        std::expected<void, Error> r;
        if (m.key == "type") {
          std::string type;
          r = String(m.value, at + ".type", type);
          if (!r) return r;
          if (type == "INSTANT") {
            effect.kind = EffectKind::kInstant;
          } else if (type == "OVERTIME") {
            effect.kind = EffectKind::kOverTime;
          } else if (type == "TEMPTIMED") {
            effect.kind = EffectKind::kTempTimed;
          } else if (type == "NULLTYPE") {
            effect.kind = std::nullopt;
          } else {
            return Fail(at + ".type",
                        std::format("unknown effect type \"{}\"", type));
          }
        } else if (m.key == "stat") {
          r = String(m.value, at + ".stat", effect.stat);
        } else if (m.key == "value") {
          r = Number(m.value, at + ".value", effect.value);
        } else if (m.key == "durationMs") {
          r = Integer(m.value, at + ".durationMs", effect.duration_ms);
        } else if (m.key == "intervalMs") {
          r = Integer(m.value, at + ".intervalMs", effect.interval_ms);
        } else {
          return Fail(at, std::format("unknown key \"{}\"", m.key));
        }
        if (!r) return r;
      }
      out.push_back(std::move(effect));
    }
    return {};
  }

  static std::expected<void, Error> Player(const json::Value& v,
                                           LegacyPlayer& out) {
    const std::string where = "player";
    if (!v.IsObject()) return Fail(where, "expected an object");
    for (const json::Member& m : v.Members()) {
      const std::string at = where + "." + m.key;
      std::expected<void, Error> r;
      if (m.key == "position") {
        Vec2 p;
        r = Point(m.value, at, p);
        out.position = p;
      } else if (m.key == "stats") {
        r = StatList(m.value, at, out.stats);
      } else if (m.key == "polling") {
        LegacyPolling p;
        r = Polling(m.value, at, p);
        out.polling = p;
      } else if (m.key == "effects") {
        std::vector<LegacyEffect> e;
        r = Effects(m.value, at, e);
        out.effects = std::move(e);
      } else {
        return Fail(where, std::format("unknown key \"{}\"", m.key));
      }
      if (!r) return r;
    }
    return {};
  }

  static std::expected<void, Error> Entity(const json::Value& v,
                                           const std::string& where,
                                           LegacyEntity& out) {
    if (!v.IsObject()) return Fail(where, "expected an object");
    for (const json::Member& m : v.Members()) {
      const std::string at = where + "." + m.key;
      std::expected<void, Error> r;
      if (m.key == "team") {
        std::string team;
        r = String(m.value, at, team);
        if (!r) return r;
        if (team == "UNAFFILIATED") {
          out.team = TeamId::kUnaffiliated;
        } else if (team == "FRIENDLY") {
          out.team = TeamId::kFriendly;
        } else if (team == "ENEMY") {
          out.team = TeamId::kEnemy;
        } else {
          return Fail(at, std::format("unknown team \"{}\"", team));
        }
      } else if (m.key == "texture") {
        r = String(m.value, at, out.texture);
      } else if (m.key == "position") {
        Vec2 p;
        r = Point(m.value, at, p);
        out.position = p;
      } else if (m.key == "rotation") {
        r = Number(m.value, at, out.rotation);
      } else if (m.key == "stats") {
        r = StatList(m.value, at, out.stats);
      } else if (m.key == "path") {
        if (!m.value.IsArray()) return Fail(at, "expected an array of [x, y]");
        for (std::size_t i = 0; i < m.value.Items().size(); ++i) {
          Vec2 p;
          r = Point(m.value.Items()[i], std::format("{}[{}]", at, i), p);
          if (!r) return r;
          out.path.push_back(p);
        }
      } else if (m.key == "repeat") {
        r = Boolean(m.value, at, out.repeat);
      } else if (m.key == "moving") {
        r = Boolean(m.value, at, out.moving);
      } else if (m.key == "polling") {
        LegacyPolling p;
        r = Polling(m.value, at, p);
        out.polling = p;
      } else if (m.key == "effects") {
        std::vector<LegacyEffect> e;
        r = Effects(m.value, at, e);
        out.effects = std::move(e);
      } else {
        return Fail(where, std::format("unknown key \"{}\"", m.key));
      }
      if (!r) return r;
    }
    return {};
  }
};

std::expected<LegacyScenario, Error> ParseScenario(std::string_view text) {
  auto doc = json::Parse(text);
  if (!doc) return std::unexpected(doc.error());
  if (!doc->IsObject()) return Fail("scenario", "must be a JSON object");
  LegacyScenario scenario;
  for (const json::Member& m : doc->Members()) {
    std::expected<void, Error> r;
    if (m.key == "format") {
      if (!m.value.IsString() || m.value.Text() != kLegacyScenarioFormat) {
        return Fail("format",
                    std::format("expected \"{}\"", kLegacyScenarioFormat));
      }
    } else if (m.key == "name") {
      r = Reader::String(m.value, "name", scenario.name);
    } else if (m.key == "description") {
      std::string description;
      r = Reader::String(m.value, "description", description);
    } else if (m.key == "eventWatchdogMs") {
      std::int64_t ms = 0;
      r = Reader::Integer(m.value, "eventWatchdogMs", ms);
      scenario.event_watchdog_ms = ms;
    } else if (m.key == "player") {
      r = Reader::Player(m.value, scenario.player);
    } else if (m.key == "entities") {
      if (!m.value.IsArray()) return Fail("entities", "expected an array");
      for (std::size_t i = 0; i < m.value.Items().size(); ++i) {
        LegacyEntity entity;
        r = Reader::Entity(m.value.Items()[i], std::format("entities[{}]", i),
                           entity);
        if (!r) return std::unexpected(r.error());
        scenario.entities.push_back(std::move(entity));
      }
    } else {
      return Fail(m.key, "unknown key");
    }
    if (!r) return std::unexpected(r.error());
  }
  return scenario;
}

std::vector<EffectDef> ToEffects(const std::vector<LegacyEffect>& effects,
                                 StatNames& names) {
  std::vector<EffectDef> out;
  for (const LegacyEffect& e : effects) {
    if (!e.kind) continue;  // NULLTYPE: no effect
    out.push_back(EffectDef{*e.kind, names.Add(e.stat), e.value, e.duration_ms,
                            e.interval_ms});
  }
  return out;
}

// config/config.json's video-settings.resolution.
std::expected<ArenaBounds, Error> ReadResolution(const std::string& root) {
  const std::string path = root + "/config/config.json";
  auto text = ReadTextFile(path);
  if (!text) return std::unexpected(text.error());
  auto doc = json::Parse(*text);
  if (!doc) return Fail(path, doc.error().message);
  const json::Value* video = doc->Find("video-settings");
  const json::Value* resolution =
      video != nullptr ? video->Find("resolution") : nullptr;
  const json::Value* width =
      resolution != nullptr ? resolution->Find("width") : nullptr;
  const json::Value* height =
      resolution != nullptr ? resolution->Find("height") : nullptr;
  const std::optional<std::int64_t> w =
      width != nullptr ? width->AsInt64() : std::nullopt;
  const std::optional<std::int64_t> h =
      height != nullptr ? height->AsInt64() : std::nullopt;
  if (!w || !h) {
    return Fail(path, "no video-settings.resolution width and height");
  }
  return ArenaBounds{static_cast<float>(*w), static_cast<float>(*h)};
}

class Textures {
 public:
  static std::expected<Textures, Error> Load(const std::string& root) {
    const std::string path = root + "/config/texture.json";
    auto text = ReadTextFile(path);
    if (!text) return std::unexpected(text.error());
    auto doc = json::Parse(*text);
    if (!doc) return Fail(path, doc.error().message);
    if (!doc->IsObject()) return Fail(path, "expected an object");
    Textures textures;
    textures.root_ = root;
    textures.config_ = std::move(*doc);
    return textures;
  }

  // The collider half extents of the texture |key|.
  std::expected<Vec2, Error> HalfExtents(const std::string& key) {
    for (const auto& [name, half] : sizes_) {
      if (name == key) return half;
    }
    const json::Value* file = config_.Find(key);
    if (file == nullptr || !file->IsString()) {
      return Fail("texture", std::format("unknown texture \"{}\"", key));
    }
    auto size = ReadPngSize(root_ + "/" + file->Text());
    if (!size) return std::unexpected(size.error());
    // R4: the texture rectangle's size, centred.
    const Vec2 half{static_cast<float>(size->width) / 2.0F,
                    static_cast<float>(size->height) / 2.0F};
    sizes_.emplace_back(key, half);
    return half;
  }

 private:
  std::string root_;
  json::Value config_;
  std::vector<std::pair<std::string, Vec2>> sizes_;
};

// The legacy Sprite: the whole texture, its origin at the centre
// (core/component/Sprite.cpp), and the collider of R4.
entt::entity CreateBase(World& world, Guid guid, Vec2 position, float rotation,
                        std::string_view texture, Vec2 half, TeamId team)
    SE_REQUIRES(g_sim_owner) {
  const entt::entity e = world.Create(guid);
  entt::registry& reg = world.Reg();
  reg.emplace<Transform>(e, position, rotation, Vec2{1.0F, 1.0F}, Tick{0});
  reg.emplace<Collider>(e, half);
  reg.emplace<Team>(e, team);
  SpriteRenderer sprite;
  sprite.texture = reg.ctx().get<TextureNames>().Add(texture);
  sprite.region = {0.0F, 0.0F, half.x * 2.0F, half.y * 2.0F};
  sprite.origin = half;
  reg.emplace<SpriteRenderer>(e, sprite);
  return e;
}

}  // namespace

std::expected<PngSize, Error> ReadPngSize(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected(
        Error{ErrorCode::kIo, "cannot open " + path, path, 0, 0});
  }
  std::array<unsigned char, 24> header{};
  in.read(reinterpret_cast<char*>(header.data()),
          static_cast<std::streamsize>(header.size()));
  constexpr std::array<unsigned char, 8> kSignature = {0x89, 'P',  'N',  'G',
                                                       '\r', '\n', 0x1A, '\n'};
  const bool signature =
      in && std::equal(kSignature.begin(), kSignature.end(), header.begin());
  if (!signature || header[12] != 'I' || header[13] != 'H' ||
      header[14] != 'D' || header[15] != 'R') {
    return std::unexpected(Error{ErrorCode::kCorrupt,
                                 path + ": not a PNG file with an IHDR chunk",
                                 path, 0, 0});
  }
  const auto be32 = [&header](std::size_t at) {
    return (static_cast<std::uint32_t>(header[at]) << 24) |
           (static_cast<std::uint32_t>(header[at + 1]) << 16) |
           (static_cast<std::uint32_t>(header[at + 2]) << 8) |
           static_cast<std::uint32_t>(header[at + 3]);
  };
  return PngSize{be32(16), be32(20)};
}

Guid LegacyGuid(std::string_view name, std::uint32_t spawn_index) noexcept {
  Hasher hasher(kLegacyGuidSeed);
  hasher.AddString(kLegacyScenarioFormat);
  hasher.AddString(name);
  const Guid parent{hasher.Finish(), 1};
  return World::DeriveGuid(parent, 0, spawn_index);
}

std::expected<LegacyScenarioSummary, Error> LoadLegacyScenario(
    World& world, const std::string& path,
    const LegacyScenarioOptions& options) {
  const auto with_path = [&path](Error error) {
    error.message = path + ": " + error.message;
    error.path = path;
    return std::unexpected(std::move(error));
  };
  auto text = ReadTextFile(path);
  if (!text) return std::unexpected(text.error());
  auto scenario = ParseScenario(*text);
  if (!scenario) return with_path(scenario.error());
  auto bounds = ReadResolution(options.root);
  if (!bounds) return std::unexpected(bounds.error());
  auto textures = Textures::Load(options.root);
  if (!textures) return std::unexpected(textures.error());

  SceneSettings settings;
  settings.bounds = *bounds;
  // Sandbox::init's watchdog of 36 s, unless the scenario sets one.
  settings.effects.event_watchdog_ms =
      scenario->event_watchdog_ms.value_or(36000);
  settings.rules = options.rules;
  ApplySceneSettings(world, settings);
  StatNames& names = world.Reg().ctx().get<StatNames>();
  entt::registry& reg = world.Reg();
  const std::string& name = scenario->name;

  // The player of Sandbox::init (spawn index 0).
  {
    auto half = textures->HalfExtents("player");
    if (!half) return std::unexpected(half.error());
    const LegacyPlayer& p = scenario->player;
    const entt::entity e = CreateBase(world, LegacyGuid(name, 0),
                                      p.position.value_or(Vec2{200.0F, 200.0F}),
                                      0.0F, "player", *half, TeamId::kFriendly);
    reg.emplace<Name>(e, "player");
    reg.emplace<PlayerControl>(e, std::uint8_t{0}, 120.0F);
    reg.emplace<MoveIntent>(e);
    reg.emplace<KinematicBody>(e);
    EffectSource source;
    source.polled = true;
    source.cooldown_ms = 1000;
    source.ready_on_start = true;
    const StatId hp = names.Add("HP");
    source.effects = {
        EffectDef{EffectKind::kInstant, hp, -10.0F, 0, 0},
        EffectDef{EffectKind::kOverTime, hp, -1.0F, 5000, 1000},
        EffectDef{EffectKind::kTempTimed, hp, -10.0F, 5000, 0},
    };
    if (p.polling) {
      source.cooldown_ms = p.polling->cooldown_ms;
      source.ready_on_start = p.polling->ready_on_start;
    }
    if (p.effects) source.effects = ToEffects(*p.effects, names);
    reg.emplace<EffectSource>(e, std::move(source));
    Stats stats;
    SetStat(stats, hp, 100.0F);
    for (const auto& [stat, value] : p.stats) {
      SetStat(stats, names.Add(stat), value);
    }
    reg.emplace<Stats>(e, std::move(stats));
  }

  // The scenario's entities (ScenarioLoader::spawn).
  for (std::size_t i = 0; i < scenario->entities.size(); ++i) {
    const LegacyEntity& le = scenario->entities[i];
    auto half = textures->HalfExtents(le.texture);
    if (!half) {
      return with_path(
          Error{ErrorCode::kNotFound,
                std::format("entities[{}]: {}", i, half.error().message),
                {},
                0,
                0});
    }
    const Vec2 position =
        le.position.value_or(le.path.empty() ? Vec2{} : le.path.front());
    const float rotation =
        le.rotation != 0.0F ? RotationFromHeading(le.rotation) : 0.0F;
    const entt::entity e =
        CreateBase(world, LegacyGuid(name, static_cast<std::uint32_t>(i + 1)),
                   position, rotation, le.texture, *half, le.team);
    Stats stats;
    for (const auto& [stat, value] : le.stats) {
      SetStat(stats, names.Add(stat), value);
    }
    reg.emplace<Stats>(e, std::move(stats));
    if (!le.path.empty()) {
      Path entity_path;
      entity_path.points = le.path;
      entity_path.loop = le.repeat;
      reg.emplace<Path>(e, std::move(entity_path));
    }
    if (le.moving) reg.emplace<Moved>(e);
    if (le.polling || le.effects) {
      EffectSource source;
      if (le.effects) source.effects = ToEffects(*le.effects, names);
      if (le.polling) {
        source.polled = true;
        source.cooldown_ms = le.polling->cooldown_ms;
        source.ready_on_start = le.polling->ready_on_start;
      }
      reg.emplace<EffectSource>(e, std::move(source));
    }
  }
  FinishLoad(world);
  return LegacyScenarioSummary{name, scenario->entities.size() + 1};
}

}  // namespace se::rampage
