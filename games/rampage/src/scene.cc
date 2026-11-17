// Copyright 2026 The simple-engine-2 Authors.

#include "game_rampage/scene.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <expected>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/json.h"
#include "game_rampage/systems.h"
#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/hash.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"

namespace se::rampage {
namespace {

std::unexpected<Error> FailAt(std::string_view where, const Error& error) {
  Error out = error;
  out.message = std::format("{}: {}", where, error.message);
  return std::unexpected(std::move(out));
}

std::unexpected<Error> FailAt(std::string_view where, std::string_view what) {
  return std::unexpected(
      Error{ErrorCode::kParse, std::format("{}: {}", where, what), {}, 0, 0});
}

// Components a scene may not set: the Guid comes from "guid", the name from
// "name", the prefab link from "prefab", and the spawn record from the load
// itself.
bool Reserved(std::string_view name) {
  return name == "IdComponent" || name == "Name" || name == "Spawned" ||
         name == "PrefabInstance";
}

// PrefabRootGuid's constants (ADR-114).
constexpr std::uint64_t kPrefabRootSeed = 0x9EFAB5007C0DE000ULL;
constexpr std::uint64_t kPrefabRootLo = 0x5EEDF00D0F1CE5EDULL;

std::expected<std::int64_t, Error> IntegerAt(const json::Value& value,
                                             std::string_view where) {
  const auto number = value.AsInt64();
  if (!number) return FailAt(where, "expected an integer");
  return *number;
}

std::expected<float, Error> FloatAt(const json::Value& value,
                                    std::string_view where) {
  const auto number = value.AsDouble();
  if (!number) return FailAt(where, "expected a number");
  return static_cast<float>(*number);
}

std::expected<SceneSettings, Error> ReadSettings(const json::Value* settings,
                                                 RulesVersion rules) {
  SceneSettings out;
  out.rules = rules;
  if (settings == nullptr) return out;
  if (!settings->IsObject()) return FailAt("settings", "expected an object");
  for (const json::Member& member : settings->Members()) {
    const std::string where = "settings." + member.key;
    if (member.key == "bounds") {
      const json::Value* w = member.value.Find("w");
      const json::Value* h = member.value.Find("h");
      if (w == nullptr || h == nullptr || member.value.Members().size() != 2) {
        return FailAt(where, "expected {\"w\": W, \"h\": H}");
      }
      auto fw = FloatAt(*w, where + ".w");
      if (!fw) return std::unexpected(fw.error());
      auto fh = FloatAt(*h, where + ".h");
      if (!fh) return std::unexpected(fh.error());
      out.bounds = {*fw, *fh};
    } else if (member.key == "seed") {
      auto seed = IntegerAt(member.value, where);
      if (!seed) return std::unexpected(seed.error());
      out.seed = static_cast<std::uint64_t>(*seed);
    } else if (member.key == "event_watchdog_ms") {
      auto ms = IntegerAt(member.value, where);
      if (!ms) return std::unexpected(ms.error());
      out.effects.event_watchdog_ms = *ms;
    } else {
      return FailAt(where, "unknown setting");
    }
  }
  return out;
}

// A stat name in an effect ("stat": "HP") becomes its id.
std::expected<void, Error> EffectStatToId(json::Value& effect, StatNames& names,
                                          std::string_view where) {
  json::Value* stat = effect.Find("stat");
  if (stat == nullptr || !stat->IsString()) return {};
  const auto id = InternStatName(names, stat->Text());
  if (!id) return FailAt(where, id.error());
  *stat = json::Value::NumberOf(static_cast<std::int64_t>(*id));
  return {};
}

// The reverse for SaveScene, when the name is known.
void EffectStatToName(json::Value& effect, const StatNames& names) {
  json::Value* stat = effect.Find("stat");
  if (stat == nullptr) return;
  const auto id = stat->AsInt64();
  if (!id) return;
  const std::string_view name = names.Name(static_cast<StatId>(*id));
  if (!name.empty()) *stat = json::Value::String(std::string(name));
}

void StatIdsToNames(std::string_view component, json::Value& value,
                    const StatNames& names) {
  if (component == "Stats") {
    const json::Value* values = value.Find("values");
    if (values == nullptr || !values->IsArray()) return;
    json::Value named = json::Value::Object();
    for (const json::Value& item : values->Items()) {
      const json::Value* id = item.Find("id");
      const json::Value* number = item.Find("value");
      if (id == nullptr || number == nullptr) return;
      const auto raw = id->AsInt64();
      if (!raw) return;
      const std::string_view name = names.Name(static_cast<StatId>(*raw));
      // An unnamed stat keeps the registry form.
      if (name.empty() || named.Find(name) != nullptr) return;
      named.Set(name, *number);
    }
    value = std::move(named);
  } else if (component == "EffectSource") {
    if (json::Value* effects = value.Find("effects")) {
      for (json::Value& effect : effects->MutableItems()) {
        EffectStatToName(effect, names);
      }
    }
  } else if (component == "ActiveEffects") {
    if (json::Value* effects = value.Find("effects")) {
      for (json::Value& effect : effects->MutableItems()) {
        if (json::Value* def = effect.Find("def")) {
          EffectStatToName(*def, names);
        }
      }
    }
  }
}

// SpriteRenderer's texture is written by name ("texture": "player", a key of
// config/texture.json) and stored as its key (TextureKeyOf).
std::expected<void, Error> TextureNameToKey(std::string_view component,
                                            json::Value& value,
                                            TextureNames& textures) {
  if (component != "SpriteRenderer") return {};
  json::Value* texture = value.Find("texture");
  if (texture == nullptr || !texture->IsString()) return {};
  const std::string name = texture->Text();
  if (name.empty()) return FailAt("texture", "empty texture name");
  const std::uint32_t key = TextureKeyOf(name);
  if (textures.Add(name) != key || textures.Name(key) != name) {
    return FailAt(name, "texture key collision");
  }
  *texture = json::Value::NumberOf(static_cast<std::int64_t>(key));
  return {};
}

// The reverse for SaveScene, when the name is known.
void TextureKeyToName(std::string_view component, json::Value& value,
                      const TextureNames& textures) {
  if (component != "SpriteRenderer") return;
  json::Value* texture = value.Find("texture");
  if (texture == nullptr) return;
  const auto key = texture->AsInt64();
  if (!key) return;
  const std::string_view name = textures.Name(static_cast<std::uint32_t>(*key));
  if (!name.empty()) *texture = json::Value::String(std::string(name));
}

// A prefab file's parts.
struct ParsedPrefab {
  std::string name;
  json::Value components = json::Value::Object();
};

std::expected<ParsedPrefab, Error> ParsePrefab(std::string_view text,
                                               std::string_view where) {
  auto doc = json::Parse(text);
  if (!doc) return FailAt(where, doc.error());
  if (!doc->IsObject()) return FailAt(where, "expected an object");
  const json::Value* format = doc->Find("format");
  if (format == nullptr || !format->IsString() ||
      format->Text() != kPrefabFormat) {
    return FailAt(where,
                  std::format("expected \"format\": \"{}\"", kPrefabFormat));
  }
  ParsedPrefab out;
  for (const json::Member& member : doc->Members()) {
    if (member.key == "format") continue;
    if (member.key == "name") {
      if (member.value.IsString()) out.name = member.value.Text();
      continue;
    }
    if (member.key == "prefab") {
      return FailAt(where, "prefabs cannot reference prefabs");
    }
    if (member.key != "components") {
      return FailAt(where, std::format("unknown key \"{}\"", member.key));
    }
    if (!member.value.IsObject()) {
      return FailAt(std::string(where) + ".components", "expected an object");
    }
    out.components = member.value;
  }
  return out;
}

std::expected<std::string, Error> ReadPrefab(std::string_view ref,
                                             const SceneLoadOptions& options) {
  return options.resolver
             ? options.resolver(ref)
             : ReadTextFile(options.asset_root + "/" + std::string(ref));
}

struct PrefabCache {
  std::map<std::string, json::Value, std::less<>> components;
};

std::expected<const json::Value*, Error> PrefabComponents(
    std::string_view ref, const SceneLoadOptions& options, PrefabCache& cache) {
  if (const auto it = cache.components.find(ref);
      it != cache.components.end()) {
    return &it->second;
  }
  const std::string where = std::format("prefab \"{}\"", ref);
  std::expected<std::string, Error> text = ReadPrefab(ref, options);
  if (!text) return FailAt(where, text.error());
  if (options.on_prefab) options.on_prefab(ref, *text);
  auto parsed = ParsePrefab(*text, where);
  if (!parsed) return std::unexpected(parsed.error());
  const auto [it, inserted] =
      cache.components.emplace(std::string(ref), std::move(parsed->components));
  return &it->second;
}

std::expected<void, Error> CreateEntity(World& world, const json::Value& entity,
                                        std::size_t index,
                                        const SceneLoadOptions& options,
                                        PrefabCache& cache, StatNames& names,
                                        TextureNames& textures)
    SE_REQUIRES(g_sim_owner) {
  const std::string where = std::format("entities[{}]", index);
  if (!entity.IsObject()) return FailAt(where, "expected an object");
  const json::Value* guid_text = entity.Find("guid");
  if (guid_text == nullptr || !guid_text->IsString()) {
    return FailAt(where, "expected \"guid\": \"<Guid>\"");
  }
  const auto guid = ParseGuid(guid_text->Text());
  if (!guid || guid->IsNil()) {
    return FailAt(where + ".guid", "not a valid, non-nil Guid");
  }
  if (world.Find(*guid)) return FailAt(where + ".guid", "duplicate Guid");

  json::Value components = json::Value::Object();
  const json::Value* overrides = nullptr;
  const std::string* prefab_ref = nullptr;
  for (const json::Member& member : entity.Members()) {
    const std::string at = where + "." + member.key;
    if (member.key == "guid" || member.key == "name") continue;
    if (member.key == "prefab") {
      if (!member.value.IsString()) return FailAt(at, "expected a path");
      auto prefab = PrefabComponents(member.value.Text(), options, cache);
      if (!prefab) return FailAt(where, prefab.error());
      prefab_ref = &member.value.Text();
      for (const json::Member& c : (*prefab)->Members()) {
        if (components.Find(c.key) == nullptr) components.Set(c.key, c.value);
      }
    } else if (member.key == "components") {
      if (!member.value.IsObject()) return FailAt(at, "expected an object");
    } else if (member.key == "overrides") {
      if (!member.value.IsObject()) return FailAt(at, "expected an object");
      overrides = &member.value;
    } else {
      return FailAt(where, std::format("unknown key \"{}\"", member.key));
    }
  }
  // An entity's own components replace its prefab's whole.
  if (const json::Value* own = entity.Find("components")) {
    for (const json::Member& c : own->Members()) components.Set(c.key, c.value);
  }
  if (overrides != nullptr) json::MergePatch(components, *overrides);

  const ComponentRegistry& registry = world.Registry();
  for (const json::Member& c : components.Members()) {
    const ComponentInfo* info = registry.FindByName(c.key);
    if (info == nullptr) {
      return FailAt(where, std::format("unknown component \"{}\"", c.key));
    }
    if (Reserved(c.key) || !HasFlag(info->flags, ComponentFlags::kSerialized)) {
      return FailAt(where, std::format("component \"{}\" cannot be set in a "
                                       "scene",
                                       c.key));
    }
  }
  for (json::Member& c : components.MutableMembers()) {
    if (auto named = StatNamesToIds(c.key, c.value, names); !named) {
      return FailAt(where + ".components." + c.key, named.error());
    }
    if (auto keyed = TextureNameToKey(c.key, c.value, textures); !keyed) {
      return FailAt(where + ".components." + c.key, keyed.error());
    }
  }

  const entt::entity e = world.Create(*guid);
  entt::registry& reg = world.Reg();
  if (const json::Value* name = entity.Find("name")) {
    if (!name->IsString()) return FailAt(where + ".name", "expected a string");
    reg.emplace<Name>(e, name->Text());
  }
  if (prefab_ref != nullptr) reg.emplace<PrefabInstance>(e, *prefab_ref);
  // Registration order: Transform before the gameplay components.
  for (const ComponentInfo& info : registry.All()) {
    const json::Value* value = components.Find(info.name);
    if (value == nullptr) continue;
    const std::string text = json::Write(*value);
    if (auto read = info.read_json(reg, e, text); !read) {
      return FailAt(where + ".components." + std::string(info.name),
                    read.error());
    }
  }
  // R12: an entity on a path starts at its first point unless the scene
  // places it.
  const json::Value* transform = components.Find("Transform");
  const auto* path = reg.try_get<Path>(e);
  if (path != nullptr && !path->points.empty() && transform != nullptr &&
      transform->Find("pos") == nullptr) {
    reg.get<Transform>(e).pos = path->points.front();
  }
  return {};
}

}  // namespace

std::expected<std::string, Error> ReadTextFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected(
        Error{ErrorCode::kIo, "cannot open " + path, path, 0, 0});
  }
  std::string text((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  return text;
}

void ApplySceneSettings(World& world, const SceneSettings& settings) {
  auto& ctx = world.Reg().ctx();
  ctx.insert_or_assign(settings.bounds);
  ctx.insert_or_assign(settings.effects);
  ctx.insert_or_assign(SceneSeed{settings.seed});
  ctx.insert_or_assign(RulesConfig{settings.rules});
  if (ctx.find<StatNames>() == nullptr) ctx.emplace<StatNames>();
  if (ctx.find<TextureNames>() == nullptr) ctx.emplace<TextureNames>();
}

namespace {

// What FinishLoad derives, by source: written once here, beside FinishLoad,
// so RemovedWith and the load never disagree (a test checks every
// component FinishLoad adds is listed under its source).
struct DerivedComponents {
  std::string_view source;
  std::span<const std::string_view> derived;
};

constexpr std::string_view kFromPath[] = {"PathCache"};
constexpr std::string_view kFromStats[] = {"ActiveEffects"};
constexpr std::string_view kFromEffectSource[] = {"PairCooldowns"};
constexpr DerivedComponents kDerivedComponents[] = {
    {"Path", kFromPath},
    {"Stats", kFromStats},
    {"EffectSource", kFromEffectSource},
};

// The identity, the spawn record, the prefab link and the pose.
constexpr std::array<std::string_view, 5> kNotRemovable = {
    "IdComponent", "Name", "Spawned", "PrefabInstance", "Transform"};

}  // namespace

void FinishLoad(World& world) {
  entt::registry& reg = world.Reg();
  for (const auto [e, path] : reg.view<const Path>().each()) {
    BuildPathCache(path, reg.get_or_emplace<PathCache>(e));
  }
  for (const entt::entity e : reg.view<const Stats>()) {
    static_cast<void>(reg.get_or_emplace<ActiveEffects>(e));
  }
  for (const auto [e, source] : reg.view<const EffectSource>().each()) {
    if (source.polled) static_cast<void>(reg.get_or_emplace<PairCooldowns>(e));
  }
  world.FinalizeSpawns();
}

std::expected<StatId, Error> InternStatName(StatNames& names,
                                            std::string_view name) {
  if (name.empty()) return Fail(ErrorCode::kParse, "empty stat name");
  const StatId id = names.Add(name);
  if (names.Name(id) != name) {
    return Fail(ErrorCode::kParse,
                std::format("the stat names \"{}\" and \"{}\" have the same id",
                            names.Name(id), name));
  }
  return id;
}

std::expected<void, Error> StatNamesToIds(std::string_view component,
                                          json::Value& value,
                                          StatNames& names) {
  if (component == "Stats") {
    if (!value.IsObject()) return FailAt("Stats", "expected an object");
    // The registry form; the component's JSON hook sorts its values and
    // rejects repeated ids.
    const json::Value* values = value.Find("values");
    if (values != nullptr && values->IsArray()) return {};
    std::vector<std::pair<StatId, json::Value>> stats;
    for (const json::Member& member : value.Members()) {
      if (!member.value.IsNumber()) {
        return FailAt(member.key, "a stat's value must be a number");
      }
      const auto id = InternStatName(names, member.key);
      if (!id) return FailAt("Stats", id.error());
      stats.emplace_back(*id, member.value);
    }
    std::sort(stats.begin(), stats.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (std::size_t i = 1; i < stats.size(); ++i) {
      if (stats[i].first == stats[i - 1].first) {
        return FailAt("Stats", "stat id collision");
      }
    }
    json::Value list = json::Value::Array();
    for (auto& [id, number] : stats) {
      json::Value item = json::Value::Object();
      item.Set("id", json::Value::NumberOf(static_cast<std::int64_t>(id)));
      item.Set("value", std::move(number));
      list.Push(std::move(item));
    }
    json::Value out = json::Value::Object();
    out.Set("values", std::move(list));
    value = std::move(out);
    return {};
  }
  if (component == "EffectSource") {
    if (json::Value* effects = value.Find("effects")) {
      for (std::size_t i = 0; i < effects->MutableItems().size(); ++i) {
        auto done = EffectStatToId(effects->MutableItems()[i], names,
                                   std::format("effects[{}]", i));
        if (!done) return done;
      }
    }
    return {};
  }
  if (component == "ActiveEffects") {
    if (json::Value* effects = value.Find("effects")) {
      for (std::size_t i = 0; i < effects->MutableItems().size(); ++i) {
        json::Value* def = effects->MutableItems()[i].Find("def");
        if (def == nullptr) continue;
        auto done =
            EffectStatToId(*def, names, std::format("effects[{}].def", i));
        if (!done) return done;
      }
    }
  }
  return {};
}

std::expected<SceneSummary, Error> LoadScene(World& world,
                                             std::string_view text,
                                             const SceneLoadOptions& options) {
  auto doc = json::Parse(text);
  if (!doc) return std::unexpected(doc.error());
  if (!doc->IsObject()) return FailAt("scene", "expected an object");
  const json::Value* format = doc->Find("format");
  if (format == nullptr || !format->IsString() ||
      format->Text() != kSceneFormat) {
    return FailAt("format",
                  std::format("expected \"format\": \"{}\"", kSceneFormat));
  }
  SceneSummary summary;
  const json::Value* entities = nullptr;
  for (const json::Member& member : doc->Members()) {
    if (member.key == "format" || member.key == "settings") continue;
    if (member.key == "name") {
      if (!member.value.IsString()) return FailAt("name", "expected a string");
      summary.name = member.value.Text();
    } else if (member.key == "entities") {
      if (!member.value.IsArray()) {
        return FailAt("entities", "expected an array");
      }
      entities = &member.value;
    } else {
      return FailAt(member.key, "unknown key");
    }
  }
  auto settings = ReadSettings(doc->Find("settings"), options.rules);
  if (!settings) return std::unexpected(settings.error());
  ApplySceneSettings(world, *settings);
  StatNames& names = world.Reg().ctx().get<StatNames>();
  TextureNames& textures = world.Reg().ctx().get<TextureNames>();
  PrefabCache cache;
  if (entities != nullptr) {
    for (std::size_t i = 0; i < entities->Items().size(); ++i) {
      if (auto made = CreateEntity(world, entities->Items()[i], i, options,
                                   cache, names, textures);
          !made) {
        return std::unexpected(made.error());
      }
    }
    summary.entities = entities->Items().size();
  }
  FinishLoad(world);
  return summary;
}

std::expected<SceneSummary, Error> LoadSceneFile(
    World& world, const std::string& path, const SceneLoadOptions& options) {
  auto text = ReadTextFile(path);
  if (!text) return std::unexpected(text.error());
  auto summary = LoadScene(world, *text, options);
  if (!summary) {
    Error error = summary.error();
    error.message = path + ": " + error.message;
    error.path = path;
    return std::unexpected(std::move(error));
  }
  return summary;
}

Guid PrefabRootGuid(std::string_view ref) noexcept {
  Hasher hasher(kPrefabRootSeed);
  hasher.AddString(ref);
  Guid guid;
  guid.hi = hasher.Finish();
  hasher.AddU64(kPrefabRootLo);
  guid.lo = hasher.Finish();
  if (guid.IsNil()) guid.lo = 1;
  return guid;
}

std::span<const std::string_view> RemovedWith(
    std::string_view component) noexcept {
  for (const DerivedComponents& entry : kDerivedComponents) {
    if (entry.source == component) return entry.derived;
  }
  return {};
}

bool Removable(std::string_view component) noexcept {
  return std::ranges::find(kNotRemovable, component) == kNotRemovable.end();
}

namespace {

// A component FinishLoad derives from another (RemovedWith's table).
bool Derived(std::string_view component) {
  for (const DerivedComponents& entry : kDerivedComponents) {
    if (std::ranges::find(entry.derived, component) != entry.derived.end()) {
      return true;
    }
  }
  return false;
}

// The writer's first pass leaves these out (ADR-115): the pose, which R12
// may place, what FinishLoad derives, and what the ticks set. The second
// pass writes whatever of them still differs.
bool LeftOutOfFirstPass(std::string_view component) {
  return component == "Transform" || component == "MoveIntent" ||
         component == "ActionBuffer" || component == "Moved" ||
         Derived(component);
}

// What scenes, prefabs and EntityComponentsJson carry: the serialized
// components except the reserved ones.
bool Content(const ComponentInfo& info) {
  return HasFlag(info.flags, ComponentFlags::kSerialized) &&
         !Reserved(info.name);
}

bool Holds(const entt::registry& reg, const ComponentInfo& info,
           entt::entity e) {
  const auto* storage = reg.storage(info.id);
  return storage != nullptr && storage->contains(e);
}

// A World's stat and texture names, empty when it has none.
class SceneNames {
 public:
  explicit SceneNames(const World& world)
      : stats_(world.CReg().ctx().find<StatNames>()),
        textures_(world.CReg().ctx().find<TextureNames>()) {}

  [[nodiscard]] const StatNames& Stats() const {
    return stats_ != nullptr ? *stats_ : no_stats_;
  }
  [[nodiscard]] const TextureNames& Textures() const {
    return textures_ != nullptr ? *textures_ : no_textures_;
  }

 private:
  const StatNames* stats_;
  const TextureNames* textures_;
  StatNames no_stats_;
  TextureNames no_textures_;
};

// One component in the scene form: the registry's JSON with stats and
// textures by name where |names| knows them.
std::expected<json::Value, Error> SceneForm(const ComponentInfo& info,
                                            const entt::registry& reg,
                                            entt::entity e,
                                            const SceneNames& names) {
  auto text = info.write_json(reg, e);
  if (!text) return std::unexpected(text.error());
  auto value = json::Parse(*text);
  if (!value) return std::unexpected(value.error());
  StatIdsToNames(info.name, *value, names.Stats());
  TextureKeyToName(info.name, *value, names.Textures());
  return value;
}

// Every entity in spawn order.
std::vector<entt::entity> SpawnOrder(const entt::registry& reg) {
  std::vector<std::pair<std::uint32_t, entt::entity>> order;
  for (const auto [e, spawned] : reg.view<const Spawned>().each()) {
    order.emplace_back(spawned.spawn_serial, e);
  }
  std::sort(order.begin(), order.end());
  std::vector<entt::entity> out;
  out.reserve(order.size());
  for (const auto& [serial, e] : order) out.push_back(e);
  return out;
}

json::Value SceneDocument(std::string_view name, json::Value settings,
                          json::Value entities) {
  json::Value doc = json::Value::Object();
  doc.Set("format", json::Value::String(std::string(kSceneFormat)));
  doc.Set("name", json::Value::String(std::string(name)));
  if (!settings.IsNull()) doc.Set("settings", std::move(settings));
  doc.Set("entities", std::move(entities));
  return doc;
}

json::Value SettingsOf(const World& world) {
  const auto& ctx = world.CReg().ctx();
  const auto* bounds = ctx.find<ArenaBounds>();
  const auto* effects = ctx.find<EffectSettings>();
  const auto* seed = ctx.find<SceneSeed>();
  json::Value settings = json::Value::Object();
  const ArenaBounds b = bounds != nullptr ? *bounds : ArenaBounds{};
  json::Value bounds_value = json::Value::Object();
  bounds_value.Set("w", json::Value::NumberOf(static_cast<double>(b.w)));
  bounds_value.Set("h", json::Value::NumberOf(static_cast<double>(b.h)));
  settings.Set("bounds", std::move(bounds_value));
  settings.Set("seed", json::Value::NumberOf(static_cast<std::int64_t>(
                           seed != nullptr ? seed->value : 0)));
  settings.Set("event_watchdog_ms",
               json::Value::NumberOf(effects != nullptr
                                         ? effects->event_watchdog_ms
                                         : EffectSettings{}.event_watchdog_ms));
  return settings;
}

// An entity's "guid" and "name" members.
json::Value EntityHeader(const entt::registry& reg, entt::entity e) {
  json::Value entity = json::Value::Object();
  entity.Set("guid",
             json::Value::String(ToString(reg.get<IdComponent>(e).guid)));
  if (const auto* entity_name = reg.try_get<Name>(e)) {
    entity.Set("name", json::Value::String(entity_name->value));
  }
  return entity;
}

// An entity with its components whole (kFlatten, and kPrefabs for entities
// without a prefab).
std::expected<json::Value, Error> FlatEntity(const World& world, entt::entity e,
                                             const SceneNames& names) {
  const entt::registry& reg = world.CReg();
  json::Value entity = EntityHeader(reg, e);
  json::Value components = json::Value::Object();
  for (const ComponentInfo& info : world.Registry().All()) {
    if (!Content(info) || !Holds(reg, info, e)) continue;
    auto value = SceneForm(info, reg, e, names);
    if (!value) return std::unexpected(value.error());
    components.Set(info.name, std::move(*value));
  }
  entity.Set("components", std::move(components));
  return entity;
}

// {"guid", "prefab", "overrides"} for a scratch load.
json::Value InstanceEntity(Guid guid, std::string_view ref,
                           const json::Value* overrides) {
  json::Value entity = json::Value::Object();
  entity.Set("guid", json::Value::String(ToString(guid)));
  entity.Set("prefab", json::Value::String(std::string(ref)));
  if (overrides != nullptr && !overrides->Members().empty()) {
    entity.Set("overrides", *overrides);
  }
  return entity;
}

// Loads |doc| into a new World of |registry|. The World is this call's
// own: the calling thread creates and so owns it (World checks that at run
// time), and nothing else can reach it, so filling it needs no g_sim_owner
// (the ownership of the running Worlds, which this function does not
// touch). ADR-116.
std::expected<std::unique_ptr<World>, Error> ScratchLoad(
    const ComponentRegistry& registry, const json::Value& doc,
    const SceneLoadOptions& options) SE_NO_THREAD_SAFETY_ANALYSIS {
  auto world = std::make_unique<World>(registry, WorldDesc{.name = "scratch"});
  auto loaded = LoadScene(*world, json::Write(doc), options);
  if (!loaded) return std::unexpected(loaded.error());
  return world;
}

// The options of the scratch loads of one call: nothing reported, and each
// prefab read once.
SceneLoadOptions ScratchOptions(const SceneLoadOptions& options) {
  // Shared by every copy of the resolver (only the pointer is copied).
  struct Cache {
    SceneLoadOptions source;
    std::map<std::string, std::string, std::less<>> texts;
  };
  SceneLoadOptions out = options;
  out.on_prefab = nullptr;
  auto cache = std::make_shared<Cache>();
  cache->source = options;
  cache->source.on_prefab = nullptr;
  out.resolver =
      [cache](std::string_view ref) -> std::expected<std::string, Error> {
    if (const auto it = cache->texts.find(ref); it != cache->texts.end()) {
      return it->second;
    }
    auto text = ReadPrefab(ref, cache->source);
    if (text) cache->texts.emplace(std::string(ref), *text);
    return text;
  };
  return out;
}

// |options| with |ref| resolving to |text|.
SceneLoadOptions WithPrefabText(const SceneLoadOptions& options,
                                std::string_view ref, std::string_view text) {
  SceneLoadOptions out = options;
  out.resolver =
      [base = options.resolver, ref = std::string(ref),
       text = std::string(text)](
          std::string_view wanted) -> std::expected<std::string, Error> {
    if (wanted == ref) return text;
    return base(wanted);
  };
  return out;
}

// |patch|'s members in registration order.
json::Value InRegistrationOrder(const ComponentRegistry& registry,
                                const json::Value& patch) {
  json::Value out = json::Value::Object();
  for (const ComponentInfo& info : registry.All()) {
    if (const json::Value* value = patch.Find(info.name)) {
      out.Set(info.name, *value);
    }
  }
  return out;
}

struct Instance {
  entt::entity entity = entt::null;
  Guid guid;
  std::string ref;
};

std::vector<Instance> InstancesIn(const World& world,
                                  std::span<const entt::entity> order) {
  const entt::registry& reg = world.CReg();
  std::vector<Instance> out;
  for (const entt::entity e : order) {
    if (const auto* link = reg.try_get<PrefabInstance>(e)) {
      out.push_back({e, reg.get<IdComponent>(e).guid, link->prefab});
    }
  }
  return out;
}

// Each instance's "overrides" (ADR-115), so that loading the instance from
// its prefab and them gives the entity. Pass 1: the scene form of each
// component against a bare instance's (equal: nothing; absent from the
// prefab: the whole value; absent from the entity: null; otherwise the
// merge diff), leaving out what LeftOutOfFirstPass names. Pass 2: the
// instances loaded with pass 1's overrides; whatever still differs is
// written as its diff against the loaded value (a component pass 1 wrote:
// as its diff against the prefab's own text, which rebuilds it exactly).
std::expected<std::vector<json::Value>, Error> ComputeOverrides(
    const World& world, std::span<const Instance> instances,
    const SceneLoadOptions& options) {
  std::vector<json::Value> out(instances.size(), json::Value::Object());
  if (instances.empty()) return out;
  const entt::registry& reg = world.CReg();
  const ComponentRegistry& registry = world.Registry();
  const SceneNames names(world);

  // The prefabs' own components, and one bare instance of each.
  std::map<std::string, json::Value, std::less<>> authored;
  json::Value bare = json::Value::Array();
  for (const Instance& instance : instances) {
    if (authored.contains(instance.ref)) continue;
    const std::string where = std::format("prefab \"{}\"", instance.ref);
    auto text = ReadPrefab(instance.ref, options);
    if (!text) return FailAt(where, text.error());
    auto parsed = ParsePrefab(*text, where);
    if (!parsed) return std::unexpected(parsed.error());
    authored.emplace(instance.ref, std::move(parsed->components));
    bare.Push(
        InstanceEntity(PrefabRootGuid(instance.ref), instance.ref, nullptr));
  }
  auto bare_world = ScratchLoad(
      registry, SceneDocument("bare", json::Value(), std::move(bare)), options);
  if (!bare_world) return std::unexpected(bare_world.error());
  const World& base = **bare_world;
  const SceneNames base_names(base);

  for (std::size_t i = 0; i < instances.size(); ++i) {
    const entt::entity e = instances[i].entity;
    const entt::entity b = *base.Find(PrefabRootGuid(instances[i].ref));
    for (const ComponentInfo& info : registry.All()) {
      if (!Content(info) || LeftOutOfFirstPass(info.name)) continue;
      const bool mine = Holds(reg, info, e);
      const bool theirs = Holds(base.CReg(), info, b);
      if (!mine) {
        if (theirs) out[i].Set(info.name, json::Value());
        continue;
      }
      auto value = SceneForm(info, reg, e, names);
      if (!value) return std::unexpected(value.error());
      if (!theirs) {
        out[i].Set(info.name, std::move(*value));
        continue;
      }
      auto prefab_value = SceneForm(info, base.CReg(), b, base_names);
      if (!prefab_value) return std::unexpected(prefab_value.error());
      if (auto patch = json::MergeDiff(*prefab_value, *value)) {
        out[i].Set(info.name, std::move(*patch));
      }
    }
  }

  json::Value loaded = json::Value::Array();
  for (std::size_t i = 0; i < instances.size(); ++i) {
    loaded.Push(InstanceEntity(instances[i].guid, instances[i].ref, &out[i]));
  }
  auto loaded_world = ScratchLoad(
      registry, SceneDocument("loaded", json::Value(), std::move(loaded)),
      options);
  if (!loaded_world) return std::unexpected(loaded_world.error());
  const World& scratch = **loaded_world;
  const SceneNames scratch_names(scratch);

  for (std::size_t i = 0; i < instances.size(); ++i) {
    const entt::entity e = instances[i].entity;
    const entt::entity s = *scratch.Find(instances[i].guid);
    json::Value& overrides = out[i];
    bool changed = false;
    for (const ComponentInfo& info : registry.All()) {
      if (!Content(info)) continue;
      const bool mine = Holds(reg, info, e);
      const bool theirs = Holds(scratch.CReg(), info, s);
      if (!mine && !theirs) continue;
      if (mine && theirs) {
        auto a = info.write_json(reg, e);
        auto c = info.write_json(scratch.CReg(), s);
        if (!a) return std::unexpected(a.error());
        if (!c) return std::unexpected(c.error());
        if (*a == *c) continue;
      }
      changed = true;
      if (!mine) {
        overrides.Set(info.name, json::Value());
        continue;
      }
      auto value = SceneForm(info, reg, e, names);
      if (!value) return std::unexpected(value.error());
      if (!theirs) {
        overrides.Set(info.name, std::move(*value));
        continue;
      }
      if (overrides.Find(info.name) != nullptr) {
        const json::Value* own = authored.at(instances[i].ref).Find(info.name);
        if (auto patch = json::MergeDiff(own != nullptr ? *own : json::Value(),
                                         *value)) {
          overrides.Set(info.name, std::move(*patch));
        }
        continue;
      }
      auto loaded_value = SceneForm(info, scratch.CReg(), s, scratch_names);
      if (!loaded_value) return std::unexpected(loaded_value.error());
      if (auto patch = json::MergeDiff(*loaded_value, *value)) {
        overrides.Set(info.name, std::move(*patch));
      }
    }
    if (changed) overrides = InRegistrationOrder(registry, overrides);
  }
  return out;
}

// One entity's components in the registry's JSON form (EntityComponentsJson).
std::expected<EntityComponentsJson, Error> ComponentsOf(const World& world,
                                                        entt::entity e) {
  const entt::registry& reg = world.CReg();
  EntityComponentsJson out;
  out.guid = reg.get<IdComponent>(e).guid;
  for (const ComponentInfo& info : world.Registry().All()) {
    if (!Content(info) || !Holds(reg, info, e)) continue;
    auto text = info.write_json(reg, e);
    if (!text) return std::unexpected(text.error());
    out.components.emplace_back(std::string(info.name), std::move(*text));
  }
  return out;
}

SceneLoadOptions LoadOptionsFor(const World& world,
                                const SceneWriteOptions& options) {
  SceneLoadOptions out;
  out.asset_root = options.asset_root;
  out.resolver = options.resolver;
  if (const auto* rules = world.CReg().ctx().find<RulesConfig>()) {
    out.rules = rules->version;
  }
  return ScratchOptions(out);
}

// A prefab document's text as loaded (its components object), so that
// SavePrefabDocument keeps the file's own form where nothing changed
// (ADR-116).
struct PrefabDocumentSource {
  json::Value components;
};

}  // namespace

std::expected<std::string, Error> SaveScene(const World& world,
                                            std::string_view name,
                                            const SceneWriteOptions& options) {
  const entt::registry& reg = world.CReg();
  const SceneNames names(world);
  const std::vector<entt::entity> order = SpawnOrder(reg);
  std::vector<Instance> instances;
  std::vector<json::Value> overrides;
  if (options.mode == SceneWriteMode::kPrefabs) {
    instances = InstancesIn(world, order);
    auto computed =
        ComputeOverrides(world, instances, LoadOptionsFor(world, options));
    if (!computed) return std::unexpected(computed.error());
    overrides = std::move(*computed);
  }

  json::Value list = json::Value::Array();
  std::size_t next = 0;
  for (const entt::entity e : order) {
    if (next < instances.size() && instances[next].entity == e) {
      json::Value entity = EntityHeader(reg, e);
      entity.Set("prefab", json::Value::String(instances[next].ref));
      if (!overrides[next].Members().empty()) {
        entity.Set("overrides", std::move(overrides[next]));
      }
      list.Push(std::move(entity));
      ++next;
      continue;
    }
    auto entity = FlatEntity(world, e, names);
    if (!entity) return std::unexpected(entity.error());
    list.Push(std::move(*entity));
  }
  const json::Value doc =
      SceneDocument(name, SettingsOf(world), std::move(list));
  return json::Write(doc, 2) + "\n";
}

std::expected<SceneSummary, Error> LoadPrefabDocument(
    World& world, std::string_view text, std::string_view ref,
    const SceneLoadOptions& options) {
  const std::string where = std::format("prefab \"{}\"", ref);
  auto parsed = ParsePrefab(text, where);
  if (!parsed) return std::unexpected(parsed.error());
  SceneSettings settings;
  settings.rules = options.rules;
  ApplySceneSettings(world, settings);
  json::Value entity = json::Value::Object();
  entity.Set("guid", json::Value::String(ToString(PrefabRootGuid(ref))));
  if (!parsed->name.empty()) {
    entity.Set("name", json::Value::String(parsed->name));
  }
  entity.Set("components", parsed->components);
  StatNames& names = world.Reg().ctx().get<StatNames>();
  TextureNames& textures = world.Reg().ctx().get<TextureNames>();
  PrefabCache cache;
  if (auto made =
          CreateEntity(world, entity, 0, options, cache, names, textures);
      !made) {
    return FailAt(where, made.error());
  }
  FinishLoad(world);
  world.Reg().ctx().insert_or_assign(
      PrefabDocumentSource{std::move(parsed->components)});
  return SceneSummary{parsed->name, 1};
}

std::expected<std::string, Error> SavePrefabDocument(const World& world,
                                                     std::string_view ref,
                                                     std::string_view name) {
  const Guid root_guid = PrefabRootGuid(ref);
  const std::optional<entt::entity> root = world.Find(root_guid);
  if (!root) {
    return Fail(
        ErrorCode::kNotFound,
        std::format("prefab \"{}\": the World has no root entity", ref));
  }
  const entt::registry& reg = world.CReg();
  const SceneNames names(world);
  const auto* source = reg.ctx().find<PrefabDocumentSource>();

  // What the document's own text loads to, to keep its form.
  std::unique_ptr<World> loaded;
  std::optional<entt::entity> loaded_root;
  if (source != nullptr) {
    json::Value entity = json::Value::Object();
    entity.Set("guid", json::Value::String(ToString(root_guid)));
    entity.Set("components", source->components);
    json::Value list = json::Value::Array();
    list.Push(std::move(entity));
    SceneLoadOptions options;
    if (const auto* rules = reg.ctx().find<RulesConfig>()) {
      options.rules = rules->version;
    }
    auto scratch = ScratchLoad(
        world.Registry(),
        SceneDocument("source", json::Value(), std::move(list)), options);
    if (!scratch) return std::unexpected(scratch.error());
    loaded = std::move(*scratch);
    loaded_root = loaded->Find(root_guid);
  }

  json::Value components = json::Value::Object();
  for (const ComponentInfo& info : world.Registry().All()) {
    if (!Content(info) || !Holds(reg, info, *root)) continue;
    const json::Value* own =
        source != nullptr ? source->components.Find(info.name) : nullptr;
    // A derived component is the load's, unless the file wrote it.
    if (own == nullptr && Derived(info.name)) continue;
    auto value = SceneForm(info, reg, *root, names);
    if (!value) return std::unexpected(value.error());
    if (own != nullptr && loaded_root &&
        Holds(loaded->CReg(), info, *loaded_root)) {
      auto was =
          SceneForm(info, loaded->CReg(), *loaded_root, SceneNames(*loaded));
      if (!was) return std::unexpected(was.error());
      json::Value kept = *own;
      if (auto patch = json::MergeDiff(*was, *value)) {
        json::MergePatch(kept, *patch);
      }
      components.Set(info.name, std::move(kept));
    } else {
      components.Set(info.name, std::move(*value));
    }
  }

  json::Value doc = json::Value::Object();
  doc.Set("format", json::Value::String(std::string(kPrefabFormat)));
  if (!name.empty()) doc.Set("name", json::Value::String(std::string(name)));
  doc.Set("components", std::move(components));
  return json::Write(doc, 2) + "\n";
}

std::expected<EntityComponentsJson, Error> PrefabBaseline(
    const World& world, Guid guid, const SceneLoadOptions& options) {
  const std::optional<entt::entity> e = world.Find(guid);
  if (!e) {
    return Fail(ErrorCode::kNotFound,
                std::format("no entity {}", ToString(guid)));
  }
  const auto* link = world.CReg().try_get<PrefabInstance>(*e);
  if (link == nullptr) {
    return Fail(
        ErrorCode::kInvalidArgument,
        std::format("entity {} is not a prefab instance", ToString(guid)));
  }
  json::Value list = json::Value::Array();
  list.Push(InstanceEntity(guid, link->prefab, nullptr));
  auto bare = ScratchLoad(world.Registry(),
                          SceneDocument("bare", json::Value(), std::move(list)),
                          ScratchOptions(options));
  if (!bare) return std::unexpected(bare.error());
  return ComponentsOf(**bare, *(*bare)->Find(guid));
}

std::expected<std::vector<EntityComponentsJson>, Error> RebasePrefabInstances(
    const World& world, std::string_view ref, std::string_view old_text,
    std::string_view new_text, const SceneLoadOptions& options) {
  std::vector<Instance> instances;
  for (Instance& instance : InstancesIn(world, SpawnOrder(world.CReg()))) {
    if (instance.ref == ref) instances.push_back(std::move(instance));
  }
  std::vector<EntityComponentsJson> out;
  if (instances.empty()) return out;
  const SceneLoadOptions scratch = ScratchOptions(options);
  auto overrides = ComputeOverrides(world, instances,
                                    WithPrefabText(scratch, ref, old_text));
  if (!overrides) return std::unexpected(overrides.error());
  json::Value list = json::Value::Array();
  for (std::size_t i = 0; i < instances.size(); ++i) {
    list.Push(InstanceEntity(instances[i].guid, ref, &(*overrides)[i]));
  }
  auto rebased =
      ScratchLoad(world.Registry(),
                  SceneDocument("rebased", json::Value(), std::move(list)),
                  WithPrefabText(scratch, ref, new_text));
  if (!rebased) return std::unexpected(rebased.error());
  for (const Instance& instance : instances) {
    auto now = ComponentsOf(world, instance.entity);
    if (!now) return std::unexpected(now.error());
    auto then = ComponentsOf(**rebased, *(*rebased)->Find(instance.guid));
    if (!then) return std::unexpected(then.error());
    if (then->components != now->components) out.push_back(std::move(*then));
  }
  return out;
}

}  // namespace se::rampage
