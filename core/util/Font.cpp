#include "Font.hpp"

bool Font::loadFromFile(const std::filesystem::path& filename)
{
    return openFromFile(filename);
}

bool Font::loadFromMemory(const void* data, std::size_t sizeInBytes)
{
    return openFromMemory(data, sizeInBytes);
}

bool Font::loadFromStream(sf::InputStream& stream)
{
    return openFromStream(stream);
}
