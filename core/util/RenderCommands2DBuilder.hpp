#pragma once

#include <entt/entity/registry.hpp>

#include "Components.hpp"
#include "util/RenderCommands2D.hpp"

/**
 * Convenience helpers for building RenderCommands2D from common engine components.
 * Intended to be called from the simulation thread (safe to read the registry).
 */
namespace RenderCommands2DBuilder
{
inline void emitSprites(const entt::registry& reg, RenderCommands2D& out)
{
    const auto view = reg.view<Sprite>();
    for (const auto entity : view)
    {
        const auto& sp = view.get<Sprite>(entity);

        SpriteDrawCmd cmd;
        cmd.texture = &sp.getTexture();
        cmd.texRect = sp.getTextureRect();
        cmd.position = sp.getPosition();
        cmd.scale = sp.getScale();
        cmd.origin = sp.getOrigin();
        cmd.color = sp.getColor();
        cmd.rotationDeg = sp.getRotation().asDegrees();
        out.sprites.push_back(cmd);
    }
}
} // namespace RenderCommands2DBuilder

