#pragma once

#include "serializer/DataStoreSerializerBase.hpp"
#include "util/DataStore.hpp"
#include "util/Logger.hpp"
#include <any>
#include <expected>
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

    std::expected<DataStore<>, bool> load(std::string_view filename) override;
    std::expected<DataStore<>, bool> save(std::string_view filename) override;
    std::expected<DataStore<>, bool> update(std::string_view filename) override;
};

