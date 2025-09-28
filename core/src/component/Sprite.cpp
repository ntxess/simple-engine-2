#include "Sprite.hpp"

Sprite::Sprite(sf::Texture& texture, bool repeat)
{
    texture.setRepeated(repeat);
    sprite.setTexture(texture);
    sprite.setOrigin(sprite.getLocalBounds().width * 0.5f, sprite.getLocalBounds().height * 0.5f);
}

Sprite::Sprite(sf::Texture& texture, int width, int height, bool repeat)
{
    texture.setRepeated(repeat);
    sprite.setTexture(texture);
    sprite.setTextureRect(sf::IntRect(0, 0, width, height));
    sprite.setOrigin(sprite.getLocalBounds().width / 2.f, sprite.getLocalBounds().height / 2.f);
}

void Sprite::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}