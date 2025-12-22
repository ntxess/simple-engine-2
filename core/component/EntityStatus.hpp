#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include <entt/entity/entity.hpp>

#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"
#include "util/TransparentStringHash.hpp"

class EntityStatus : public IComponent
{
public:
    void accept(IComponentVisitor* visitor, entt::entity entityID) override;

    // NOTE: This is on hot paths (movement/collision/events). Use transparent lookup to avoid
    // constructing temporary std::string for queries (e.g. find(std::string_view)).
    std::unordered_map<std::string, float, TransparentStringHash, TransparentStringEqual> values;

    inline bool has(std::string_view key) const noexcept
    {
        return values.find(key) != values.end();
    }

    inline float getOr(std::string_view key, float fallback = 0.f) const noexcept
    {
        const auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }

    inline float& ref(std::string_view key, float initial = 0.f)
    {
        auto [it, inserted] = values.try_emplace(std::string{ key }, initial);
        return it->second;
    }
};