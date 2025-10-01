#include <SFML/Graphics/Font.hpp>

class Font : public sf::Font
{
public:
    using sf::Font::Font;
    // Changed function identity to match other loadFrom* functions
    // using sf::Font::openFromFile;
    // using sf::Font::openFromMemory;
    // using sf::Font::openFromStream;
    using sf::Font::getGlyph;
    using sf::Font::getInfo;
    using sf::Font::getKerning;
    using sf::Font::getLineSpacing;
    using sf::Font::getTexture;
    using sf::Font::getUnderlinePosition;
    using sf::Font::getUnderlineThickness;
    using sf::Font::isSmooth;
    using sf::Font::setSmooth;

    bool loadFromFile(const std::filesystem::path& filename);
    bool loadFromMemory(const void* data, std::size_t sizeInBytes);
    bool loadFromStream(sf::InputStream& stream);
};