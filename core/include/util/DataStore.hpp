#pragma once

#include "util/Logger.hpp"
#include "util/AnyToString.hpp"
#include <any>
#include <ostream>
#include <optional>
#include <string>
#include <unordered_map>

struct TransparentHash
{
    using is_transparent = void;

    size_t operator()(std::string_view str) const noexcept
    {
        return std::hash<std::string_view>{}(str);
    }

    size_t operator()(const std::string& str) const noexcept
    {
        return std::hash<std::string>{}(str);
    }
};

struct TransparentEqual
{
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept
    {
        return lhs == rhs;
    }

    bool operator()(const std::string& lhs, const std::string& rhs) const noexcept
    {
        return lhs == rhs;
    }

    bool operator()(const std::string& lhs, std::string_view rhs) const noexcept
    {
        return lhs == rhs;
    }

    bool operator()(std::string_view lhs, const std::string& rhs) const noexcept
    {
        return lhs == rhs;
    }
};

class DataStore
{
public:
    using key_type = std::string;
    using mapped_type = std::any;
    using container_type = std::unordered_map<key_type, mapped_type, TransparentHash, TransparentEqual>;
    using iterator = container_type::iterator;
    using const_iterator = container_type::const_iterator;

    template<typename T>
    std::optional<T> get(std::string_view key) const;

    template<typename T>
    void set(std::string_view key, T&& val);

    bool contains(std::string_view key) const;
    void remove(std::string_view key);
    void clear();

    container_type& data();
    const container_type& data() const;

    const_iterator find(std::string_view key) const;
    iterator begin();
    iterator end();
    const_iterator begin() const;
    const_iterator end() const;

private:
    std::unordered_map<key_type, mapped_type, TransparentHash, TransparentEqual> m_data;
};

template<typename T>
inline std::optional<T> DataStore::get(std::string_view key) const
{
    if (auto it = m_data.find(key); it != m_data.end())
    {
        try
        {
            return std::any_cast<T>(it->second);
        }
        catch (const std::bad_any_cast& e)
        {
            LOG_ERROR(Logger::get()) << "Failed to cast value for key: " << key << " | [" << getType(m_data.at(std::string(key))) << ", " << getValue(m_data.at(std::string(key))) << "]. Error: " << e.what();
            return std::nullopt;
        }
    }
    return std::nullopt;
}

template<typename T>
inline void DataStore::set(std::string_view key, T&& value)
{
    m_data[std::string(key)] = std::forward<T>(value);
}

inline std::ostream& operator<<(std::ostream& os, const DataStore& ds)
{
    for (const auto& [key, val] : ds)
    {
        os << "\t{ " << key << ", " << getValue(val) << " } | Type: " << getType(val) << "\n";
    }

    return os;
}
