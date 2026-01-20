#pragma once

#include <toml++/toml.h>

#include <any>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "serializer/DataStoreSerializerBase.hpp"
#include "util/DataStore.hpp"
#include "util/Logger.hpp"

/**
 * TOML-backed DataStore serializer (toml++).
 *
 * Notes:
 * - Nested tables are flattened into dotted keys (e.g. window.width -> "window.width").
 * - Arrays are stored as std::vector<std::any> and may be heterogenous.
 * - Provides overloads for writing/updating a file from a caller-provided DataStore.
 */
class TomlDataStoreSerializer : public DataStoreSerializerBase
{
public:
    TomlDataStoreSerializer();
    explicit TomlDataStoreSerializer(std::filesystem::path path);
    ~TomlDataStoreSerializer() override = default;

    std::expected<DataStore<>, bool> load(std::string_view filename) override;

    // Base interface has no DataStore parameter; keep these for interface compatibility.
    // Prefer the overloads below.
    std::expected<DataStore<>, bool> save(std::string_view filename) override;
    std::expected<DataStore<>, bool> update(std::string_view filename) override;

    // Preferred APIs: write/update using caller-provided data.
    std::expected<void, bool> save(std::string_view filename, const DataStore<>& dataStore);
    std::expected<void, bool> update(std::string_view filename, const DataStore<>& updates);

private:
    void readTable(const toml::table& tbl, std::string_view prefix, DataStore<>& out);
    std::optional<std::any> nodeToAny(const toml::node& node);

    bool setDottedKey(toml::table& root, std::string_view dottedKey, const std::any& value);
    toml::table* ensureTablePath(toml::table& root, std::string_view dottedKeyPrefix);

    bool insertAny(toml::table& tbl, std::string_view key, const std::any& value);
    std::optional<toml::array> anyVectorToTomlArray(const std::vector<std::any>& vec);
    std::optional<toml::table> anyDataStoreToTomlTable(const DataStore<>& ds);
};

