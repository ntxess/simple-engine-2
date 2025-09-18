#include "GameDataStoreSerializer.hpp"

GameDataStoreSerializer::GameDataStoreSerializer()
    : IDataStoreSerializer()
{}

GameDataStoreSerializer::GameDataStoreSerializer(std::filesystem::path path)
    : IDataStoreSerializer(path)
{}

std::expected<DataStore, bool> GameDataStoreSerializer::load(std::string_view filename)
{
    return std::expected<DataStore, bool>();
}

std::expected<DataStore, bool> GameDataStoreSerializer::save(std::string_view filename)
{
    return std::expected<DataStore, bool>();
}

std::expected<DataStore, bool> GameDataStoreSerializer::update(std::string_view filename)
{
    return std::expected<DataStore, bool>();
}