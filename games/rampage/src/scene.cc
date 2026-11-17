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
  std::expected<std::string, Error> text =
      options.resolver
          ? options.resolver(ref)
          : ReadTextFile(options.asset_root + "/" + std::string(ref));
  if (!text) return FailAt(where, text.error());
  if (options.on_prefab) options.on_prefab(ref, *text);
  auto doc = json::Parse(*text);
  if (!doc) return FailAt(where, doc.error());
  if (!doc->IsObject()) return FailAt(where, "expected an object");
  const json::Value* format = doc->Find("format");
  if (format == nullptr || !format->IsString() ||
      format->Text() != kPrefabFormat) {
    return FailAt(where,
                  std::format("expected \"format\": \"{}\"", kPrefabFormat));
  }
  json::Value components = json::Value::Object();
  for (const json::Member& member : doc->Members()) {
    if (member.key == "format" || member.key == "name") continue;
    if (member.key == "prefab") {
      return FailAt(where, "prefabs cannot reference prefabs");
    }
    if (member.key != "components") {
      return FailAt(where, std::format("unknown key \"{}\"", member.key));
    }
    if (!member.value.IsObject()) {
      return FailAt(where + ".components", "expected an object");
    }
    components = member.value;
  }
  const auto [it, inserted] =
      cache.components.emplace(std::string(ref), std::move(components));
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

std::expected<std::string, Error> SaveScene(const World& world,
                                            std::string_view name,
                                            const SceneWriteOptions& options) {
  if (options.mode != SceneWriteMode::kFlatten) {
    return Fail(ErrorCode::kUnsupported, "kPrefabs: not yet (M11a)");
  }
  const entt::registry& reg = world.CReg();
  const auto& ctx = reg.ctx();
  const auto* bounds = ctx.find<ArenaBounds>();
  const auto* effects = ctx.find<EffectSettings>();
  const auto* seed = ctx.find<SceneSeed>();
  const auto* stat_names = ctx.find<StatNames>();
  const StatNames no_names;
  const StatNames& names = stat_names != nullptr ? *stat_names : no_names;
  const auto* texture_names = ctx.find<TextureNames>();
  const TextureNames no_textures;
  const TextureNames& textures =
      texture_names != nullptr ? *texture_names : no_textures;

  json::Value doc = json::Value::Object();
  doc.Set("format", json::Value::String(std::string(kSceneFormat)));
  doc.Set("name", json::Value::String(std::string(name)));
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
  doc.Set("settings", std::move(settings));

  std::vector<std::pair<std::uint32_t, entt::entity>> order;
  for (const auto [e, spawned] : reg.view<const Spawned>().each()) {
    order.emplace_back(spawned.spawn_serial, e);
  }
  std::sort(order.begin(), order.end());

  json::Value list = json::Value::Array();
  for (const auto& [serial, e] : order) {
    json::Value entity = json::Value::Object();
    entity.Set("guid",
               json::Value::String(ToString(reg.get<IdComponent>(e).guid)));
    if (const auto* entity_name = reg.try_get<Name>(e)) {
      entity.Set("name", json::Value::String(entity_name->value));
    }
    json::Value components = json::Value::Object();
    for (const ComponentInfo& info : world.Registry().All()) {
      if (Reserved(info.name) ||
          !HasFlag(info.flags, ComponentFlags::kSerialized)) {
        continue;
      }
      const auto* storage = reg.storage(info.id);
      if (storage == nullptr || !storage->contains(e)) continue;
      auto text = info.write_json(reg, e);
      if (!text) return std::unexpected(text.error());
      auto value = json::Parse(*text);
      if (!value) return std::unexpected(value.error());
      StatIdsToNames(info.name, *value, names);
      TextureKeyToName(info.name, *value, textures);
      components.Set(info.name, std::move(*value));
    }
    entity.Set("components", std::move(components));
    list.Push(std::move(entity));
  }
  doc.Set("entities", std::move(list));
  return json::Write(doc, 2) + "\n";
}

// M11a stubs: the scene contract's declarations build; the bodies follow.

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

std::expected<SceneSummary, Error> LoadPrefabDocument(
    World& world, std::string_view text, std::string_view ref,
    const SceneLoadOptions& options) {
  static_cast<void>(world);
  static_cast<void>(text);
  static_cast<void>(ref);
  static_cast<void>(options);
  return Fail(ErrorCode::kUnsupported, "LoadPrefabDocument: not yet (M11a)");
}

std::expected<std::string, Error> SavePrefabDocument(const World& world,
                                                     std::string_view ref,
                                                     std::string_view name) {
  static_cast<void>(world);
  static_cast<void>(ref);
  static_cast<void>(name);
  return Fail(ErrorCode::kUnsupported, "SavePrefabDocument: not yet (M11a)");
}

std::expected<EntityComponentsJson, Error> PrefabBaseline(
    const World& world, Guid guid, const SceneLoadOptions& options) {
  static_cast<void>(world);
  static_cast<void>(guid);
  static_cast<void>(options);
  return Fail(ErrorCode::kUnsupported, "PrefabBaseline: not yet (M11a)");
}

std::expected<std::vector<EntityComponentsJson>, Error> RebasePrefabInstances(
    const World& world, std::string_view ref, std::string_view old_text,
    std::string_view new_text, const SceneLoadOptions& options) {
  static_cast<void>(world);
  static_cast<void>(ref);
  static_cast<void>(old_text);
  static_cast<void>(new_text);
  static_cast<void>(options);
  return Fail(ErrorCode::kUnsupported, "RebasePrefabInstances: not yet (M11a)");
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

}  // namespace se::rampage
