#pragma once

#include "serializer/DataStoreSerializerBase.hpp"
#include "util/DataStore.hpp"
#include "util/Logger.hpp"
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <any>
#include <fstream>
#include <filesystem>
#include <optional>
#include <string>

class JsonDataStoreSerializer : public DataStoreSerializerBase
{
public:
    JsonDataStoreSerializer();
    JsonDataStoreSerializer(std::filesystem::path path);
    ~JsonDataStoreSerializer() override = default;

    bool load(std::string_view filename, DataStore& dataStore) override;
    bool save(std::string_view filename, const DataStore& dataStore) override;
    bool update(std::string_view filename, const DataStore& dataStore) override;

private:
    void read(std::string_view key, rapidjson::Value& val, DataStore& dataStore);
    void write(rapidjson::Document& doc, const DataStore& dataStore);
    void findAndReplace(rapidjson::Document& doc, rapidjson::Value& val, const DataStore& dataStore);
    std::optional<std::any> valueToAny(const rapidjson::Value& val);
    std::optional<rapidjson::Value> createJsonValue(rapidjson::Document& doc, const std::any& data);
    void vecParseHelper(std::string_view key, rapidjson::Value& val, std::vector<std::any>& vec);
};

