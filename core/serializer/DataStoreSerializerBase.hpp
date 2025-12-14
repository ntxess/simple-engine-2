#pragma once

#include <any>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "util/DataStore.hpp"
#include "util/Logger.hpp"

class DataStoreSerializerBase
{
public:
    DataStoreSerializerBase();
    DataStoreSerializerBase(std::filesystem::path path);
    virtual ~DataStoreSerializerBase() = default;

    virtual std::expected<DataStore<>, bool> load(std::string_view filename) = 0;
    virtual std::expected<DataStore<>, bool> save(std::string_view filename) = 0;
    virtual std::expected<DataStore<>, bool> update(std::string_view filename) = 0;
    virtual std::filesystem::path resolvePath(std::string path);

protected:
    const std::filesystem::path RELATIVE_PATH;
};