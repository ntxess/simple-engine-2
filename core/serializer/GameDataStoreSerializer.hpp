#pragma once

#include <any>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "serializer/DataStoreSerializerBase.hpp"
#include "util/DataStore.hpp"
#include "util/Logger.hpp"

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

