#include "Sprite.hpp"

Sprite::Sprite(const sf::Texture& texture)
    : sf::Sprite{texture}
{
    setOrigin({ getLocalBounds().size.x / 2.f, getLocalBounds().size.y / 2.f });
}

Sprite::Sprite(const sf::Texture& texture, int width, int height)
    : sf::Sprite{texture, sf::Rect<int>{{0, 0}, {width, height}}}
{
    setOrigin({ getLocalBounds().size.x / 2.f, getLocalBounds().size.y / 2.f });
}

Sprite::Sprite(const sf::Texture& texture, const allocator_type& alloc)
    : Sprite{texture} 
{}

Sprite::Sprite(const sf::Texture& texture, int width, int height, const allocator_type& alloc)
    : Sprite{texture, width, height} 
{}

Sprite::Sprite(std::allocator_arg_t, const allocator_type& alloc, const sf::Texture& texture)
    : Sprite{texture}
{}

Sprite::Sprite(std::allocator_arg_t, const allocator_type& alloc, const sf::Texture& texture, int width, int height)
    : Sprite{texture, width, height} 
{}

Sprite::Sprite(const Sprite& other, const allocator_type& alloc)
    : sf::Sprite{other}
    , IComponent{other}
{
    setOrigin({ getLocalBounds().size.x / 2.f, getLocalBounds().size.y / 2.f });
}

Sprite::Sprite(Sprite&& other, const allocator_type& alloc) noexcept
    : sf::Sprite{std::move(other)}
    , IComponent{std::move(other)}
{
    setOrigin({ getLocalBounds().size.x / 2.f, getLocalBounds().size.y / 2.f });
}

void Sprite::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}