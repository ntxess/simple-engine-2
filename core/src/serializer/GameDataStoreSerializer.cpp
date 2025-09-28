#include "GameDataStoreSerializer.hpp"

GameDataStoreSerializer::GameDataStoreSerializer()
    : DataStoreSerializerBase()
{}

GameDataStoreSerializer::GameDataStoreSerializer(std::filesystem::path path)
    : DataStoreSerializerBase(path)
{}

bool GameDataStoreSerializer::load(std::string_view filename, DataStore& dataStore)
{
    return false;
}

bool GameDataStoreSerializer::save(std::string_view filename, const DataStore& dataStore)
{
    return false;
}

bool GameDataStoreSerializer::update(std::string_view filename, const DataStore& dataStore)
{
    return false;
}
