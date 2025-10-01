#pragma once

#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"
#include "entt/entity/entity.hpp"
#include <SFML/Graphics/RenderTexture.hpp>

class SceneViewRenderer : public IComponent, public sf::RenderTexture
{
public:
    using sf::RenderTexture::operator=;
    using sf::RenderTexture::setSmooth;
    using sf::RenderTexture::isSmooth;
    using sf::RenderTexture::setRepeated;
    using sf::RenderTexture::isRepeated;
    using sf::RenderTexture::generateMipmap;
    using sf::RenderTexture::setActive;
    using sf::RenderTexture::display;
    using sf::RenderTexture::getSize;
    using sf::RenderTexture::isSrgb;
    using sf::RenderTexture::getTexture;
    using sf::RenderTexture::clear;
    using sf::RenderTexture::clearStencil;
    using sf::RenderTexture::setView;
    using sf::RenderTexture::getView;
    using sf::RenderTexture::getDefaultView;
    using sf::RenderTexture::getViewport;
    using sf::RenderTexture::getScissor;
    using sf::RenderTexture::mapPixelToCoords;
    using sf::RenderTexture::mapCoordsToPixel;
    using sf::RenderTexture::draw;
    using sf::RenderTexture::pushGLStates;
    using sf::RenderTexture::popGLStates;
    using sf::RenderTexture::resetGLStates;
    using allocator_type = std::allocator<SceneViewRenderer>;

    SceneViewRenderer(unsigned int width, unsigned int height, const sf::ContextSettings& settings = sf::ContextSettings{});
    SceneViewRenderer(unsigned int width, unsigned int height, const sf::ContextSettings& settings, const allocator_type& alloc);
    
    // Allocator-aware forms - required by EnTT
    SceneViewRenderer(unsigned int width, unsigned int height, const allocator_type& alloc);
    SceneViewRenderer(std::allocator_arg_t, const allocator_type& alloc, unsigned int width, unsigned int height, const sf::ContextSettings& settings = sf::ContextSettings{});
    
    // Copy/Move with allocator
    SceneViewRenderer(const SceneViewRenderer& other, const allocator_type& alloc) = delete;
    SceneViewRenderer(SceneViewRenderer&& other, const allocator_type& alloc) noexcept;

    void accept(IComponentVisitor* visitor, entt::entity entityID) override;
};