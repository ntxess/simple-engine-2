#pragma once

#include "util/DataStore.hpp"
#include "util/Logger.hpp"
#include <any>
#include <fstream>
#include <filesystem>
#include <string>

class DataStore;

class DataStoreSerializerBase
{
public:
    DataStoreSerializerBase();
    DataStoreSerializerBase(std::filesystem::path path);
    virtual ~DataStoreSerializerBase() = default;

    virtual bool load(std::string_view filename, DataStore& dataStore) = 0;
    virtual bool save(std::string_view filename, const DataStore& dataStore) = 0;
    virtual bool update(std::string_view filename, const DataStore& dataStore) = 0;
    virtual std::filesystem::path resolvePath(std::string path);

protected:
    const std::filesystem::path RELATIVE_PATH;
};