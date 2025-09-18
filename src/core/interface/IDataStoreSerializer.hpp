#pragma once

#include "../util/DataStore.hpp"
#include "../util/Logger.hpp"
#include <any>
#include <expected>
#include <fstream>
#include <filesystem>
#include <string>

class DataStore;

class IDataStoreSerializer
{
public:
    IDataStoreSerializer();
    IDataStoreSerializer(std::filesystem::path path);
    virtual ~IDataStoreSerializer() = default;

    virtual std::expected<DataStore, bool> load(std::string_view filename) = 0;
    virtual std::expected<DataStore, bool> save(std::string_view filename) = 0;
    virtual std::expected<DataStore, bool> update(std::string_view filename) = 0;

    virtual std::filesystem::path resolvePath(std::string path);

protected:
    const std::filesystem::path RELATIVE_PATH;
};