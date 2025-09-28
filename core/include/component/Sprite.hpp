#pragma once

#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"
#include "entt/entity/entity.hpp"
#include <SFML/Graphics/Sprite.hpp>
#include <SFML/Graphics/Texture.hpp>

class Sprite : public IComponent
{
public:
    Sprite() = delete;
    Sprite(sf::Texture& texture, bool repeat = false);
    Sprite(sf::Texture& texture, int width, int height, bool repeat = false);

    void accept(IComponentVisitor* visitor, entt::entity entityID) override;

    inline const sf::Vector2f& getOrigin() const
    {
        return sprite.getOrigin();
    }

    inline const sf::Vector2f& getPosition() const
    {
        return sprite.getPosition();
    }

    inline float getRotation() const
    {
        return sprite.getRotation();
    }

    inline const sf::Vector2f& getScale() const
    {
        return sprite.getScale();
    }

    inline void move(float offsetX, float offsetY)
    {
        sprite.move(offsetX, offsetY);
    }

    inline void rotate(float angle)
    {
        sprite.rotate(angle);
    }

    inline void scale(float factorX, float factorY)
    {
        sprite.scale(factorX, factorY);
    }

    inline void setOrigin(float x, float y)
    {
        sprite.setOrigin(x, y);
    }

    inline void setPosition(float x, float y)
    {
        sprite.setPosition(x, y);
    }

    inline void setRotation(float angle)
    {
        sprite.setRotation(angle);
    }

    inline void setScale(float factorX, float factorY)
    {
        sprite.setScale(factorX, factorY);
    }

    inline void setTexture(const sf::Texture& texture, bool resetRect = false)
    {
        sprite.setTexture(texture, resetRect);
    }

    inline void setTextureRect(const sf::IntRect& rectangle)
    {
        sprite.setTextureRect(rectangle);
    }

    inline sf::FloatRect getGlobalBounds() const
    {
        return sprite.getGlobalBounds();
    }

    inline sf::FloatRect getLocalBounds() const
    {
        return sprite.getLocalBounds();
    }

    inline void setColor(const sf::Color& color)
    {
        sprite.setColor(color);
    }

    inline const sf::Texture* getTexture() const
    {
        return sprite.getTexture();
    }

    inline const sf::IntRect& getTextureRect() const
    {
        return sprite.getTextureRect();
    }

    inline const sf::Color& getColor() const
    {
        return sprite.getColor();
    }

    sf::Sprite sprite;
};