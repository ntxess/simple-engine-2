#include "GameDataStoreSerializer.hpp"

GameDataStoreSerializer::GameDataStoreSerializer()
    : DataStoreSerializerBase{}
{}

GameDataStoreSerializer::GameDataStoreSerializer(std::filesystem::path path)
    : DataStoreSerializerBase{path}
{}

std::expected<DataStore<>, bool> GameDataStoreSerializer::load(std::string_view filename)
{
    return std::unexpected(false);
}

std::expected<DataStore<>, bool> GameDataStoreSerializer::save(std::string_view filename)
{
    return std::unexpected(false);
}

std::expected<DataStore<>, bool> GameDataStoreSerializer::update(std::string_view filename)
{
    return std::unexpected(false);
}
