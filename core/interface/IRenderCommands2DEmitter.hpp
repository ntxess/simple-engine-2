#pragma once

#include <SFML/Graphics/RenderTarget.hpp>

#include "util/RenderCommands2D.hpp"

/**
 * Optional interface for scenes that want engine-driven 2D rendering.
 *
 * - buildRenderCommands2D() is called on the simulation thread (safe to read the registry).
 * - renderOverlay() is called on the render thread after the batch has been flushed (optional UI/debug).
 *
 * If a scene implements this interface, the engine will prefer this pipeline over calling IScene::render().
 */
class IRenderCommands2DEmitter
{
public:
    virtual ~IRenderCommands2DEmitter() = default;

    virtual void buildRenderCommands2D(RenderCommands2D& out) const = 0;

    virtual void renderOverlay(sf::RenderTarget& target) const
    {
        (void)target;
    }
};

