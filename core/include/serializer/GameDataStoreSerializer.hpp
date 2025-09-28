#pragma once

#include "serializer/DataStoreSerializerBase.hpp"
#include "util/Logger.hpp"
#include <any>
#include <fstream>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

class GameDataStoreSerializer : public DataStoreSerializerBase
{
public:
    GameDataStoreSerializer();
    GameDataStoreSerializer(std::filesystem::path path);
    ~GameDataStoreSerializer() override = default;

    bool load(std::string_view filename, DataStore& dataStore) override;
    bool save(std::string_view filename, const DataStore& dataStore) override;
    bool update(std::string_view filename, const DataStore& dataStore) override;
};

