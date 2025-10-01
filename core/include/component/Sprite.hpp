#pragma once

#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"
#include "entt/entity/entity.hpp"
#include <SFML/Graphics/Sprite.hpp>
#include <SFML/Graphics/Texture.hpp>

class Sprite : public IComponent, public sf::Sprite
{
public:
    using sf::Sprite::setTexture;
    using sf::Sprite::setTextureRect;
    using sf::Sprite::setColor;
    using sf::Sprite::getTexture;
    using sf::Sprite::getTextureRect;
    using sf::Sprite::getLocalBounds;
    using sf::Sprite::getGlobalBounds;
    using sf::Sprite::setPosition;
    using sf::Sprite::setRotation;
    using sf::Sprite::setScale;
    using sf::Sprite::setOrigin;
    using sf::Sprite::getPosition;
    using sf::Sprite::getRotation;
    using sf::Sprite::getScale;
    using sf::Sprite::getOrigin;
    using sf::Sprite::move;
    using sf::Sprite::rotate;
    using sf::Sprite::scale;
    using sf::Sprite::getTransform;
    using sf::Sprite::getInverseTransform;
    using allocator_type = std::allocator<Sprite>;

    Sprite() = delete;
    Sprite(const sf::Texture& texture);
    Sprite(const sf::Texture& texture, int width, int height);

    // Allocator-aware forms - required by EnTT
    Sprite(const sf::Texture& texture, const allocator_type& alloc);
    Sprite(const sf::Texture& texture, int width, int height, const allocator_type& alloc);
    Sprite(std::allocator_arg_t, const allocator_type& alloc, const sf::Texture& texture);
    Sprite(std::allocator_arg_t, const allocator_type& alloc, const sf::Texture& texture, int width, int height);

    // Copy/Move with allocator
    Sprite(const Sprite& other, const allocator_type& alloc);
    Sprite(Sprite&& other, const allocator_type& alloc) noexcept;

    void accept(IComponentVisitor* visitor, entt::entity entityID) override;
};