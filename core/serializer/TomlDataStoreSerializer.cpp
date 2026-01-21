#include "TomlDataStoreSerializer.hpp"

#include <climits>
#include <filesystem>
#include <fstream>
#include <string>

TomlDataStoreSerializer::TomlDataStoreSerializer()
    : DataStoreSerializerBase{}
{}

TomlDataStoreSerializer::TomlDataStoreSerializer(std::filesystem::path path)
    : DataStoreSerializerBase{std::move(path)}
{}

std::expected<DataStore<>, bool> TomlDataStoreSerializer::load(std::string_view filename)
{
    const auto path = resolvePath(filename.data());

    if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path))
    {
        LOG_ERROR(Logger::get()) << "File cannot be found: " << path.c_str();
        return std::unexpected(false);
    }

    if (std::filesystem::is_empty(path))
    {
        LOG_ERROR(Logger::get()) << "File is empty: " << path.c_str();
        return std::unexpected(false);
    }

    DataStore<> out;
    try
    {
        const toml::table root = toml::parse_file(path.string());
        readTable(root, /*prefix*/"", out);
        return out;
    }
    catch (const toml::parse_error& err)
    {
        LOG_ERROR(Logger::get()) << "Failed to parse TOML file: " << path.c_str()
                                 << " | Error: " << err.description();
        return std::unexpected(false);
    }
}

std::expected<DataStore<>, bool> TomlDataStoreSerializer::save(std::string_view filename)
{
    LOG_ERROR(Logger::get()) << "TomlDataStoreSerializer::save(filename) requires a DataStore. "
                                "Use save(filename, dataStore) instead.";
    return std::unexpected(false);
}

std::expected<DataStore<>, bool> TomlDataStoreSerializer::update(std::string_view filename)
{
    LOG_ERROR(Logger::get()) << "TomlDataStoreSerializer::update(filename) requires a DataStore. "
                                "Use update(filename, updates) instead.";
    return std::unexpected(false);
}

std::expected<void, bool> TomlDataStoreSerializer::save(std::string_view filename, const DataStore<>& dataStore)
{
    const auto path = resolvePath(filename.data());

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        LOG_ERROR(Logger::get()) << "Failed to create directories for: " << path.c_str()
                                 << " | " << ec.message();
        return std::unexpected(false);
    }

    toml::table root;
    for (const auto& [key, value] : dataStore.lockedView())
    {
        if (!setDottedKey(root, key, value))
        {
            LOG_ERROR(Logger::get()) << "Failed to serialize key to TOML: " << key;
        }
    }

    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.good())
    {
        LOG_ERROR(Logger::get()) << "File cannot be written: " << path.c_str();
        return std::unexpected(false);
    }

    ofs << root << "\n";
    return {};
}

std::expected<void, bool> TomlDataStoreSerializer::update(std::string_view filename, const DataStore<>& updates)
{
    const auto path = resolvePath(filename.data());

    if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path))
    {
        LOG_ERROR(Logger::get()) << "File cannot be found: " << path.c_str();
        return std::unexpected(false);
    }

    toml::table root;
    try
    {
        root = toml::parse_file(path.string());
    }
    catch (const toml::parse_error& err)
    {
        LOG_ERROR(Logger::get()) << "Failed to parse TOML file: " << path.c_str()
                                 << " | Error: " << err.description();
        return std::unexpected(false);
    }

    for (const auto& [key, value] : updates.lockedView())
    {
        if (!setDottedKey(root, key, value))
        {
            LOG_ERROR(Logger::get()) << "Failed to apply update for key: " << key;
        }
    }

    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.good())
    {
        LOG_ERROR(Logger::get()) << "File cannot be written: " << path.c_str();
        return std::unexpected(false);
    }

    ofs << root << "\n";
    return {};
}

void TomlDataStoreSerializer::readTable(const toml::table& tbl, std::string_view prefix, DataStore<>& out)
{
    for (const auto& [k, node] : tbl)
    {
        const std::string keyStr{k.str()};
        const std::string fullKey = prefix.empty() ? keyStr : (std::string(prefix) + "." + keyStr);

        if (node.is_table())
        {
            readTable(*node.as_table(), fullKey, out);
            continue;
        }

        const auto anyOpt = nodeToAny(node);
        if (!anyOpt)
        {
            LOG_ERROR(Logger::get()) << "Unsupported TOML node type for key: " << fullKey;
            continue;
        }

        out.set(fullKey, *anyOpt);
    }
}

std::optional<std::any> TomlDataStoreSerializer::nodeToAny(const toml::node& node)
{
    if (auto s = node.as_string())
        return std::string{s->get()};

    if (auto b = node.as_boolean())
        return b->get();

    if (auto i = node.as_integer())
    {
        const auto v = i->get();
        if (v >= INT_MIN && v <= INT_MAX)
            return static_cast<int>(v);
        // Keep engine-facing types stable (int/double); fall back to double for large ints.
        return static_cast<double>(v);
    }

    if (auto f = node.as_floating_point())
        return static_cast<double>(f->get());

    if (auto arr = node.as_array())
    {
        std::vector<std::any> vec;
        vec.reserve(arr->size());
        for (const auto& el : *arr)
        {
            if (auto elAny = nodeToAny(el))
                vec.emplace_back(std::move(*elAny));
            else
                return std::nullopt;
        }
        return vec;
    }

    // Inline tables inside arrays are allowed in TOML.
    if (auto t = node.as_table())
    {
        DataStore<> ds;
        readTable(*t, /*prefix*/"", ds);
        return ds;
    }

    return std::nullopt;
}

toml::table* TomlDataStoreSerializer::ensureTablePath(toml::table& root, std::string_view dottedKeyPrefix)
{
    toml::table* current = &root;
    std::size_t pos = 0;

    while (pos < dottedKeyPrefix.size())
    {
        const std::size_t dot = dottedKeyPrefix.find('.', pos);
        const std::string_view part = dottedKeyPrefix.substr(
            pos,
            dot == std::string_view::npos ? std::string_view::npos : (dot - pos)
        );

        if (part.empty())
            return nullptr;

        toml::node* childNode = current->get(part);
        if (!childNode || !childNode->is_table())
        {
            current->insert_or_assign(part, toml::table{});
            childNode = current->get(part);
        }

        auto* childTable = childNode ? childNode->as_table() : nullptr;
        if (!childTable)
            return nullptr;

        current = childTable;

        if (dot == std::string_view::npos)
            break;

        pos = dot + 1;
    }

    return current;
}

bool TomlDataStoreSerializer::setDottedKey(toml::table& root, std::string_view dottedKey, const std::any& value)
{
    const std::size_t lastDot = dottedKey.rfind('.');
    if (lastDot == std::string_view::npos)
        return insertAny(root, dottedKey, value);

    const std::string_view prefix = dottedKey.substr(0, lastDot);
    const std::string_view leaf = dottedKey.substr(lastDot + 1);
    if (leaf.empty())
        return false;

    toml::table* tbl = ensureTablePath(root, prefix);
    if (!tbl)
        return false;

    return insertAny(*tbl, leaf, value);
}

std::optional<toml::array> TomlDataStoreSerializer::anyVectorToTomlArray(const std::vector<std::any>& vec)
{
    toml::array arr;
    arr.reserve(vec.size());

    for (const auto& item : vec)
    {
        if (item.type() == typeid(const char*))
            arr.push_back(std::string(std::any_cast<const char*>(item)));
        else if (item.type() == typeid(std::string))
            arr.push_back(std::any_cast<const std::string&>(item));
        else if (item.type() == typeid(bool))
            arr.push_back(std::any_cast<bool>(item));
        else if (item.type() == typeid(int))
            arr.push_back(static_cast<std::int64_t>(std::any_cast<int>(item)));
        else if (item.type() == typeid(float))
            arr.push_back(static_cast<double>(std::any_cast<float>(item)));
        else if (item.type() == typeid(double))
            arr.push_back(std::any_cast<double>(item));
        else if (item.type() == typeid(std::vector<std::any>))
        {
            const auto& nested = std::any_cast<const std::vector<std::any>&>(item);
            auto nestedArr = anyVectorToTomlArray(nested);
            if (!nestedArr) return std::nullopt;
            arr.push_back(std::move(*nestedArr));
        }
        else if (item.type() == typeid(DataStore<>))
        {
            const auto& ds = std::any_cast<const DataStore<>&>(item);
            auto tbl = anyDataStoreToTomlTable(ds);
            if (!tbl) return std::nullopt;
            arr.push_back(std::move(*tbl));
        }
        else
        {
            return std::nullopt;
        }
    }

    return arr;
}

std::optional<toml::table> TomlDataStoreSerializer::anyDataStoreToTomlTable(const DataStore<>& ds)
{
    toml::table tbl;
    for (const auto& [key, value] : ds.lockedView())
    {
        if (!setDottedKey(tbl, key, value))
            return std::nullopt;
    }
    return tbl;
}

bool TomlDataStoreSerializer::insertAny(toml::table& tbl, std::string_view key, const std::any& value)
{
    if (value.type() == typeid(const char*))
    {
        tbl.insert_or_assign(key, std::string(std::any_cast<const char*>(value)));
        return true;
    }
    if (value.type() == typeid(std::string))
    {
        tbl.insert_or_assign(key, std::any_cast<const std::string&>(value));
        return true;
    }
    if (value.type() == typeid(bool))
    {
        tbl.insert_or_assign(key, std::any_cast<bool>(value));
        return true;
    }
    if (value.type() == typeid(int))
    {
        tbl.insert_or_assign(key, static_cast<std::int64_t>(std::any_cast<int>(value)));
        return true;
    }
    if (value.type() == typeid(float))
    {
        tbl.insert_or_assign(key, static_cast<double>(std::any_cast<float>(value)));
        return true;
    }
    if (value.type() == typeid(double))
    {
        tbl.insert_or_assign(key, std::any_cast<double>(value));
        return true;
    }
    if (value.type() == typeid(std::vector<std::any>))
    {
        const auto& vec = std::any_cast<const std::vector<std::any>&>(value);
        auto arr = anyVectorToTomlArray(vec);
        if (!arr) return false;
        tbl.insert_or_assign(key, std::move(*arr));
        return true;
    }
    if (value.type() == typeid(DataStore<>))
    {
        const auto& ds = std::any_cast<const DataStore<>&>(value);
        auto sub = anyDataStoreToTomlTable(ds);
        if (!sub) return false;
        tbl.insert_or_assign(key, std::move(*sub));
        return true;
    }

    return false;
}

