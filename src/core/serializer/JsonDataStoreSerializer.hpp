#pragma once

#include "../interface/IDataStoreSerializer.hpp"
#include "../util/DataStore.hpp"
#include "../util/Logger.hpp"
#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/prettywriter.h"
#include <any>
#include <expected>
#include <fstream>
#include <filesystem>
#include <optional>
#include <string>

class JsonDataStoreSerializer : public IDataStoreSerializer
{
public:
    JsonDataStoreSerializer();
    JsonDataStoreSerializer(std::filesystem::path path);
    ~JsonDataStoreSerializer() override = default;

    std::expected<DataStore, bool> load(std::string_view filename) override;
    std::expected<DataStore, bool> save(std::string_view filename) override;
    std::expected<DataStore, bool> update(std::string_view filename) override;

private:
    void read(std::string_view key, rapidjson::Value& val, DataStore& dataStore);
    void write(rapidjson::Document& doc, const DataStore& dataStore);
    void findAndReplace(rapidjson::Document& doc, rapidjson::Value& val, const DataStore& dataStore);
    std::optional<std::any> valueToAny(const rapidjson::Value& val);
    std::optional<rapidjson::Value> createJsonValue(rapidjson::Document& doc, const std::any& data);
    void vecParseHelper(std::string_view key, rapidjson::Value& val, std::vector<std::any>& vec);
};

