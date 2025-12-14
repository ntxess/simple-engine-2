#include "SceneViewRenderer.hpp"

SceneViewRenderer::SceneViewRenderer(unsigned int width, unsigned int height, const sf::ContextSettings& settings)
    : sf::RenderTexture{{ width, height }, settings}
{}

SceneViewRenderer::SceneViewRenderer(unsigned int width, unsigned int height, const sf::ContextSettings& settings, const allocator_type& /*alloc*/)
    : SceneViewRenderer{width, height, settings}
{}

SceneViewRenderer::SceneViewRenderer(unsigned int width, unsigned int height, const allocator_type& /*alloc*/)
    : SceneViewRenderer{width, height, sf::ContextSettings{}}
{}

SceneViewRenderer::SceneViewRenderer(std::allocator_arg_t, const allocator_type& /*alloc*/, unsigned int width, unsigned int height, const sf::ContextSettings& settings)
    : SceneViewRenderer{width, height, settings}
{}

SceneViewRenderer::SceneViewRenderer(SceneViewRenderer&& other, const allocator_type& /*alloc*/) noexcept
    : sf::RenderTexture{std::move(other)}
    , IComponent{std::move(other)}
{}

void SceneViewRenderer::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}