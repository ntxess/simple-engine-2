#include "headless/ScenarioLoader.hpp"

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>

#include <SFML/System/Angle.hpp>
#include <entt/entity/registry.hpp>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include "Components.hpp"
#include "Systems.hpp"
#include "component/SpawnIndex.hpp"
#include "scene/Sandbox.hpp"
#include "util/WayPoint.hpp"

namespace
{
    constexpr const char* scenarioFormat = "se-legacy-scenario-v1";

    const char* teamName(Team team)
    {
        switch (team)
        {
        case Team::UNAFFILIATED: return "UNAFFILIATED";
        case Team::FRIENDLY: return "FRIENDLY";
        case Team::ENEMY: return "ENEMY";
        }
        return "UNAFFILIATED";
    }

    bool parseTeam(const std::string& name, Team& team)
    {
        if (name == "UNAFFILIATED") { team = Team::UNAFFILIATED; return true; }
        if (name == "FRIENDLY") { team = Team::FRIENDLY; return true; }
        if (name == "ENEMY") { team = Team::ENEMY; return true; }
        return false;
    }

    const char* effectTypeName(EffectType type)
    {
        switch (type)
        {
        case EffectType::NULLTYPE: return "NULLTYPE";
        case EffectType::INSTANT: return "INSTANT";
        case EffectType::OVERTIME: return "OVERTIME";
        case EffectType::TEMPTIMED: return "TEMPTIMED";
        }
        return "NULLTYPE";
    }

    bool parseEffectType(const std::string& name, EffectType& type)
    {
        if (name == "NULLTYPE") { type = EffectType::NULLTYPE; return true; }
        if (name == "INSTANT") { type = EffectType::INSTANT; return true; }
        if (name == "OVERTIME") { type = EffectType::OVERTIME; return true; }
        if (name == "TEMPTIMED") { type = EffectType::TEMPTIMED; return true; }
        return false;
    }

    // Small helpers that record the first error with its JSON location.
    struct Parser
    {
        std::string error;

        bool fail(const std::string& where, const std::string& what)
        {
            if (error.empty())
                error = where + ": " + what;
            return false;
        }

        bool number(const rapidjson::Value& v, const std::string& where, double& out)
        {
            if (!v.IsNumber()) return fail(where, "expected a number");
            out = v.GetDouble();
            return true;
        }

        bool integer(const rapidjson::Value& v, const std::string& where, std::int64_t& out)
        {
            if (!v.IsInt64()) return fail(where, "expected an integer");
            out = v.GetInt64();
            return true;
        }

        bool boolean(const rapidjson::Value& v, const std::string& where, bool& out)
        {
            if (!v.IsBool()) return fail(where, "expected true or false");
            out = v.GetBool();
            return true;
        }

        bool string(const rapidjson::Value& v, const std::string& where, std::string& out)
        {
            if (!v.IsString()) return fail(where, "expected a string");
            out.assign(v.GetString(), v.GetStringLength());
            return true;
        }

        bool point(const rapidjson::Value& v, const std::string& where, sf::Vector2f& out)
        {
            if (!v.IsArray() || v.Size() != 2 || !v[0].IsNumber() || !v[1].IsNumber())
                return fail(where, "expected [x, y]");
            out = { static_cast<float>(v[0].GetDouble()), static_cast<float>(v[1].GetDouble()) };
            return true;
        }

        bool stats(const rapidjson::Value& v, const std::string& where, std::vector<std::pair<std::string, float>>& out)
        {
            if (!v.IsObject()) return fail(where, "expected an object of stat values");
            for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it)
            {
                double value = 0.0;
                const std::string name(it->name.GetString(), it->name.GetStringLength());
                if (!number(it->value, where + "." + name, value)) return false;
                out.emplace_back(name, static_cast<float>(value));
            }
            return true;
        }

        bool polling(const rapidjson::Value& v, const std::string& where, ScenarioPolling& out)
        {
            if (!v.IsObject()) return fail(where, "expected an object");
            for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it)
            {
                const std::string key = it->name.GetString();
                if (key == "cooldownMs") { if (!integer(it->value, where + ".cooldownMs", out.cooldownMs)) return false; }
                else if (key == "readyOnStart") { if (!boolean(it->value, where + ".readyOnStart", out.readyOnStart)) return false; }
                else return fail(where, "unknown key \"" + key + "\"");
            }
            return true;
        }

        bool effects(const rapidjson::Value& v, const std::string& where, std::vector<ScenarioEffect>& out)
        {
            if (!v.IsArray()) return fail(where, "expected an array of effects");
            for (rapidjson::SizeType i = 0; i < v.Size(); ++i)
            {
                const std::string at = where + "[" + std::to_string(i) + "]";
                const rapidjson::Value& e = v[i];
                if (!e.IsObject()) return fail(at, "expected an object");
                ScenarioEffect effect;
                for (auto it = e.MemberBegin(); it != e.MemberEnd(); ++it)
                {
                    const std::string key = it->name.GetString();
                    if (key == "type")
                    {
                        std::string name;
                        if (!string(it->value, at + ".type", name)) return false;
                        if (!parseEffectType(name, effect.type)) return fail(at + ".type", "unknown effect type \"" + name + "\"");
                    }
                    else if (key == "stat") { if (!string(it->value, at + ".stat", effect.stat)) return false; }
                    else if (key == "value")
                    {
                        double value = 0.0;
                        if (!number(it->value, at + ".value", value)) return false;
                        effect.value = static_cast<float>(value);
                    }
                    else if (key == "durationMs") { if (!integer(it->value, at + ".durationMs", effect.durationMs)) return false; }
                    else if (key == "intervalMs") { if (!integer(it->value, at + ".intervalMs", effect.intervalMs)) return false; }
                    else return fail(at, "unknown key \"" + key + "\"");
                }
                out.push_back(std::move(effect));
            }
            return true;
        }

        bool player(const rapidjson::Value& v, ScenarioPlayer& out)
        {
            const std::string where = "player";
            if (!v.IsObject()) return fail(where, "expected an object");
            for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it)
            {
                const std::string key = it->name.GetString();
                if (key == "position") { sf::Vector2f p; if (!point(it->value, where + ".position", p)) return false; out.position = p; }
                else if (key == "stats") { if (!stats(it->value, where + ".stats", out.stats)) return false; }
                else if (key == "polling") { ScenarioPolling p; if (!polling(it->value, where + ".polling", p)) return false; out.polling = p; }
                else if (key == "effects") { std::vector<ScenarioEffect> e; if (!effects(it->value, where + ".effects", e)) return false; out.effects = std::move(e); }
                else return fail(where, "unknown key \"" + key + "\"");
            }
            return true;
        }

        bool entity(const rapidjson::Value& v, const std::string& where, ScenarioEntity& out)
        {
            if (!v.IsObject()) return fail(where, "expected an object");
            for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it)
            {
                const std::string key = it->name.GetString();
                const std::string at = where + "." + key;
                if (key == "team")
                {
                    std::string name;
                    if (!string(it->value, at, name)) return false;
                    if (!parseTeam(name, out.team)) return fail(at, "unknown team \"" + name + "\"");
                }
                else if (key == "texture") { if (!string(it->value, at, out.texture)) return false; }
                else if (key == "position") { sf::Vector2f p; if (!point(it->value, at, p)) return false; out.position = p; }
                else if (key == "rotation") { double r = 0.0; if (!number(it->value, at, r)) return false; out.rotation = static_cast<float>(r); }
                else if (key == "stats") { if (!stats(it->value, at, out.stats)) return false; }
                else if (key == "path")
                {
                    if (!it->value.IsArray()) return fail(at, "expected an array of [x, y]");
                    for (rapidjson::SizeType i = 0; i < it->value.Size(); ++i)
                    {
                        sf::Vector2f p;
                        if (!point(it->value[i], at + "[" + std::to_string(i) + "]", p)) return false;
                        out.path.push_back(p);
                    }
                }
                else if (key == "repeat") { if (!boolean(it->value, at, out.repeat)) return false; }
                else if (key == "moving") { if (!boolean(it->value, at, out.moving)) return false; }
                else if (key == "polling") { ScenarioPolling p; if (!polling(it->value, at, p)) return false; out.polling = p; }
                else if (key == "effects") { std::vector<ScenarioEffect> e; if (!effects(it->value, at, e)) return false; out.effects = std::move(e); }
                else return fail(where, "unknown key \"" + key + "\"");
            }
            return true;
        }
    };

    void fillEffects(EffectsList& list, const std::vector<ScenarioEffect>& effects)
    {
        list.effectsList.clear();
        for (const auto& e : effects)
        {
            list.effectsList.push_back({ e.type, Effects{ e.stat, e.value, std::chrono::milliseconds(e.durationMs), std::chrono::milliseconds(e.intervalMs) } });
        }
    }

    void writeFloat(std::ostream& os, float value)
    {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(value));
        os << buf;
    }

    void writeString(std::ostream& os, const std::string& s)
    {
        os << '"';
        for (const char c : s)
        {
            if (c == '"' || c == '\\') os << '\\' << c;
            else if (c == '\n') os << "\\n";
            else os << c;
        }
        os << '"';
    }

    void writePoint(std::ostream& os, const sf::Vector2f& p)
    {
        os << '[';
        writeFloat(os, p.x);
        os << ", ";
        writeFloat(os, p.y);
        os << ']';
    }

    void writeStats(std::ostream& os, const std::vector<std::pair<std::string, float>>& stats)
    {
        os << '{';
        for (std::size_t i = 0; i < stats.size(); ++i)
        {
            if (i) os << ", ";
            writeString(os, stats[i].first);
            os << ": ";
            writeFloat(os, stats[i].second);
        }
        os << '}';
    }

    void writeEffects(std::ostream& os, const std::vector<ScenarioEffect>& effects, const char* indent)
    {
        os << '[';
        for (std::size_t i = 0; i < effects.size(); ++i)
        {
            const auto& e = effects[i];
            os << (i ? "," : "") << '\n' << indent << "  {\"type\": \"" << effectTypeName(e.type) << "\", \"stat\": ";
            writeString(os, e.stat);
            os << ", \"value\": ";
            writeFloat(os, e.value);
            os << ", \"durationMs\": " << e.durationMs << ", \"intervalMs\": " << e.intervalMs << '}';
        }
        if (!effects.empty())
            os << '\n' << indent;
        os << ']';
    }
}

bool ScenarioLoader::load(const std::string& path, Scenario& scenario, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        error = "cannot open scenario " + path;
        return false;
    }
    std::stringstream contents;
    contents << file.rdbuf();
    const std::string text = contents.str();

    rapidjson::Document doc;
    doc.Parse(text.c_str(), text.size());
    if (doc.HasParseError())
    {
        error = path + ": JSON error at offset " + std::to_string(doc.GetErrorOffset()) + ": " + rapidjson::GetParseError_En(doc.GetParseError());
        return false;
    }
    if (!doc.IsObject())
    {
        error = path + ": the scenario must be a JSON object";
        return false;
    }

    Parser p;
    scenario = Scenario{};
    for (auto it = doc.MemberBegin(); it != doc.MemberEnd() && p.error.empty(); ++it)
    {
        const std::string key = it->name.GetString();
        if (key == "format")
        {
            std::string format;
            if (p.string(it->value, "format", format) && format != scenarioFormat)
                p.fail("format", "expected \"" + std::string(scenarioFormat) + "\"");
        }
        else if (key == "name") p.string(it->value, "name", scenario.name);
        else if (key == "description") p.string(it->value, "description", scenario.description);
        else if (key == "eventWatchdogMs") { std::int64_t ms = 0; if (p.integer(it->value, "eventWatchdogMs", ms)) scenario.eventWatchdogMs = ms; }
        else if (key == "player") p.player(it->value, scenario.player);
        else if (key == "entities")
        {
            if (!it->value.IsArray()) { p.fail("entities", "expected an array"); break; }
            for (rapidjson::SizeType i = 0; i < it->value.Size() && p.error.empty(); ++i)
            {
                ScenarioEntity entity;
                if (p.entity(it->value[i], "entities[" + std::to_string(i) + "]", entity))
                    scenario.entities.push_back(std::move(entity));
            }
        }
        else p.fail(key, "unknown key");
    }

    if (!p.error.empty())
    {
        error = path + ": " + p.error;
        return false;
    }
    return true;
}

bool ScenarioLoader::save(const std::string& path, const Scenario& scenario, std::string& error)
{
    std::ostringstream os;
    os << "{\n  \"format\": \"" << scenarioFormat << "\",\n  \"name\": ";
    writeString(os, scenario.name);
    os << ",\n  \"description\": ";
    writeString(os, scenario.description);
    if (scenario.eventWatchdogMs)
        os << ",\n  \"eventWatchdogMs\": " << *scenario.eventWatchdogMs;

    const ScenarioPlayer& player = scenario.player;
    if (player.position || !player.stats.empty() || player.polling || player.effects)
    {
        os << ",\n  \"player\": {";
        const char* sep = "\n    ";
        if (player.position) { os << sep << "\"position\": "; writePoint(os, *player.position); sep = ",\n    "; }
        if (!player.stats.empty()) { os << sep << "\"stats\": "; writeStats(os, player.stats); sep = ",\n    "; }
        if (player.polling) { os << sep << "\"polling\": {\"cooldownMs\": " << player.polling->cooldownMs << ", \"readyOnStart\": " << (player.polling->readyOnStart ? "true" : "false") << '}'; sep = ",\n    "; }
        if (player.effects) { os << sep << "\"effects\": "; writeEffects(os, *player.effects, "    "); }
        os << "\n  }";
    }

    os << ",\n  \"entities\": [";
    for (std::size_t i = 0; i < scenario.entities.size(); ++i)
    {
        const ScenarioEntity& e = scenario.entities[i];
        os << (i ? ",\n" : "\n") << "    {\"team\": \"" << teamName(e.team) << "\", \"texture\": ";
        writeString(os, e.texture);
        if (e.position) { os << ", \"position\": "; writePoint(os, *e.position); }
        if (e.rotation != 0.f) { os << ", \"rotation\": "; writeFloat(os, e.rotation); }
        os << ",\n     \"stats\": ";
        writeStats(os, e.stats);
        if (!e.path.empty())
        {
            os << ",\n     \"path\": [";
            for (std::size_t j = 0; j < e.path.size(); ++j)
            {
                if (j) os << ", ";
                writePoint(os, e.path[j]);
            }
            os << "], \"repeat\": " << (e.repeat ? "true" : "false");
        }
        os << ", \"moving\": " << (e.moving ? "true" : "false");
        if (e.polling) os << ",\n     \"polling\": {\"cooldownMs\": " << e.polling->cooldownMs << ", \"readyOnStart\": " << (e.polling->readyOnStart ? "true" : "false") << '}';
        if (e.effects) { os << ",\n     \"effects\": "; writeEffects(os, *e.effects, "     "); }
        os << '}';
    }
    os << (scenario.entities.empty() ? "]\n}\n" : "\n  ]\n}\n");

    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        error = "cannot write scenario " + path;
        return false;
    }
    file << os.str();
    return static_cast<bool>(file);
}

bool ScenarioLoader::spawn(const Scenario& scenario, Sandbox& sandbox, ApplicationContext& ctx, std::string& error)
{
    entt::registry& reg = sandbox.getRegistry();
    const std::uint32_t rate = ctx.simTime.rate;
    const std::uint64_t now = ctx.simTime.tick;

    if (scenario.eventWatchdogMs)
    {
        if (EventSystem* events = sandbox.getSystemManager()->getSystem<EventSystem>())
            events->setWatchdogTicks(msToTicks(*scenario.eventWatchdogMs, rate));
    }

    const entt::entity player = sandbox.getPlayer();
    if (reg.valid(player))
    {
        const ScenarioPlayer& p = scenario.player;
        if (p.position && reg.all_of<Sprite>(player))
            reg.get<Sprite>(player).setPosition(*p.position);
        if (!p.stats.empty() && reg.all_of<EntityStatus>(player))
        {
            for (const auto& [name, value] : p.stats)
                reg.get<EntityStatus>(player).values[name] = value;
        }
        if (p.polling)
            reg.emplace_or_replace<UpdateEntityPolling>(player, msToTicks(p.polling->cooldownMs, rate), p.polling->readyOnStart, now);
        if (p.effects)
            fillEffects(reg.get_or_emplace<EffectsList>(player), *p.effects);
    }

    for (std::size_t i = 0; i < scenario.entities.size(); ++i)
    {
        const ScenarioEntity& e = scenario.entities[i];
        const auto texture = ctx.textureManager.get(e.texture);
        if (!texture)
        {
            error = "entities[" + std::to_string(i) + "]: unknown texture \"" + e.texture + "\"";
            return false;
        }

        // Components are added in the order Editor::generateEntities adds them.
        const entt::entity entity = reg.create();
        assignSpawnIndex(reg, entity);
        reg.emplace<Sprite>(entity, texture.value().get());
        const sf::Vector2f position = e.position ? *e.position : (e.path.empty() ? sf::Vector2f{} : e.path.front());
        reg.get<Sprite>(entity).setPosition(position);
        if (e.rotation != 0.f)
            reg.get<Sprite>(entity).setRotation(sf::degrees(e.rotation));
        reg.emplace<EntityStatus>(entity);
        for (const auto& [name, value] : e.stats)
            reg.get<EntityStatus>(entity).values[name] = value;
        reg.emplace<TeamTag>(entity, e.team);
        if (!e.path.empty())
            reg.emplace<MovementPattern>(entity, WayPoint::makePath(e.path), e.repeat);
        if (e.moving)
            reg.emplace<UpdateEntityEvent>(entity);
        if (e.polling)
            reg.emplace<UpdateEntityPolling>(entity, msToTicks(e.polling->cooldownMs, rate), e.polling->readyOnStart, now);
        if (e.effects)
            fillEffects(reg.emplace<EffectsList>(entity), *e.effects);
    }

    return true;
}

void ScenarioLoader::appendStressEntities(Scenario& scenario, const std::vector<std::vector<sf::Vector2f>>& paths)
{
    for (const auto& path : paths)
    {
        ScenarioEntity e;
        e.team = Team::ENEMY;
        e.texture = "player";
        e.stats = { { "HP", 100.f }, { "Speed", 0.5f } };
        e.path = path;
        e.repeat = true;
        e.moving = true;
        scenario.entities.push_back(std::move(e));
    }
}
