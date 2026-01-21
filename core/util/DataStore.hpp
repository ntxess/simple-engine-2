#pragma once

#include <any>
#include <charconv>
#include <cctype>
#include <mutex>
#include <ostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "util/AnyToString.hpp"
#include "util/Logger.hpp"
#include "util/SyncPolicy.hpp"
#include "util/TransparentStringHash.hpp"

template<typename SyncPolicy = NoSync>
class DataStore
{
public:
    using key_type = std::string;
    using mapped_type = std::any;
    using container_type = std::unordered_map<key_type, mapped_type, TransparentStringHash, TransparentStringEqual>;
    using iterator = container_type::iterator;
    using const_iterator = container_type::const_iterator;

    template<typename T>
    std::optional<T> get(std::string_view key) const;

    /**
     * Like get<T>(), but attempts basic coercions for common scalar types.
     *
     * Supported (best-effort) coercions:
     * - int  <- double/float/bool/string
     * - double <- int/float/bool/string
     * - bool <- int/double/string
     * - std::string <- int/double/bool
     *
     * Arrays/complex types are not coerced.
     */
    template<typename T>
    std::optional<T> getCoerced(std::string_view key) const;

    template<typename T>
    void set(std::string_view key, T&& val);

    bool contains(std::string_view key) const;
    void remove(std::string_view key);
    void clear();

    const_iterator find(std::string_view key) const;
    iterator begin();
    iterator end();
    const_iterator begin() const;
    const_iterator end() const;
    
    // Thread-safe view for ranged-based for loops.
    // Internally holds lock and does not make a copy.
    // Be careful of holding on resources for too long.
    struct DataStoreView
    {
        std::unique_lock<SyncPolicy> lock;
        const container_type& ref;

        auto begin() const { return ref.begin(); }
        auto end() const { return ref.end(); }
    };

    DataStoreView lockedView() const
    {
        return DataStoreView{ std::unique_lock<SyncPolicy>(m_sync), m_data };
    }

private:
    mutable SyncPolicy m_sync{};
    container_type m_data;
};

template<typename SyncPolicy>
template<typename T>
inline std::optional<T> DataStore<SyncPolicy>::get(std::string_view key) const
{
    std::unique_lock<SyncPolicy> lock(m_sync);
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

template<typename SyncPolicy>
template<typename T>
inline std::optional<T> DataStore<SyncPolicy>::getCoerced(std::string_view key) const
{
    std::unique_lock<SyncPolicy> lock(m_sync);
    const auto it = m_data.find(key);
    if (it == m_data.end())
        return std::nullopt;

    const std::any& a = it->second;

    if (auto direct = std::any_cast<T>(&a))
        return *direct;

    auto parseBool = [](std::string_view sv) -> std::optional<bool>
    {
        auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
        while (!sv.empty() && isSpace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
        while (!sv.empty() && isSpace(static_cast<unsigned char>(sv.back()))) sv.remove_suffix(1);
        if (sv.empty()) return std::nullopt;

        auto lower = [](char c) -> char { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
        std::string tmp;
        tmp.reserve(sv.size());
        for (char c : sv) tmp.push_back(lower(c));

        if (tmp == "true" || tmp == "1" || tmp == "yes" || tmp == "on")  return true;
        if (tmp == "false" || tmp == "0" || tmp == "no" || tmp == "off") return false;
        return std::nullopt;
    };

    auto parseInt = [](std::string_view sv) -> std::optional<int>
    {
        int v = 0;
        const auto* begin = sv.data();
        const auto* end = sv.data() + sv.size();
        auto [ptr, ec] = std::from_chars(begin, end, v);
        if (ec == std::errc{} && ptr == end) return v;
        return std::nullopt;
    };

    auto parseDouble = [](std::string_view sv) -> std::optional<double>
    {
        // from_chars for floating is C++17 but not universally implemented on MSVC/libstdc++.
        // Keep it simple and use stod via a temporary string.
        try
        {
            size_t idx = 0;
            const std::string tmp(sv);
            const double v = std::stod(tmp, &idx);
            if (idx == tmp.size()) return v;
            return std::nullopt;
        }
        catch (...)
        {
            return std::nullopt;
        }
    };

    if constexpr (std::is_same_v<T, int>)
    {
        if (auto p = std::any_cast<double>(&a)) return static_cast<int>(*p);
        if (auto p = std::any_cast<float>(&a))  return static_cast<int>(*p);
        if (auto p = std::any_cast<bool>(&a))   return *p ? 1 : 0;
        if (auto p = std::any_cast<std::string>(&a)) return parseInt(*p);
        return std::nullopt;
    }
    else if constexpr (std::is_same_v<T, double>)
    {
        if (auto p = std::any_cast<int>(&a))    return static_cast<double>(*p);
        if (auto p = std::any_cast<float>(&a))  return static_cast<double>(*p);
        if (auto p = std::any_cast<bool>(&a))   return *p ? 1.0 : 0.0;
        if (auto p = std::any_cast<std::string>(&a)) return parseDouble(*p);
        return std::nullopt;
    }
    else if constexpr (std::is_same_v<T, bool>)
    {
        if (auto p = std::any_cast<int>(&a))    return (*p != 0);
        if (auto p = std::any_cast<double>(&a)) return (*p != 0.0);
        if (auto p = std::any_cast<std::string>(&a)) return parseBool(*p);
        return std::nullopt;
    }
    else if constexpr (std::is_same_v<T, std::string>)
    {
        if (auto p = std::any_cast<int>(&a))    return std::to_string(*p);
        if (auto p = std::any_cast<float>(&a))  return std::to_string(*p);
        if (auto p = std::any_cast<double>(&a)) return std::to_string(*p);
        if (auto p = std::any_cast<bool>(&a))   return (*p ? "true" : "false");
        return std::nullopt;
    }

    return std::nullopt;
}

template<typename SyncPolicy>
template<typename T>
inline void DataStore<SyncPolicy>::set(std::string_view key, T&& value)
{
    std::unique_lock<SyncPolicy> lock(m_sync);
    m_data[std::string(key)] = std::forward<T>(value);
}

template<typename SyncPolicy>
bool DataStore<SyncPolicy>::contains(std::string_view key) const
{
    std::unique_lock<SyncPolicy> lock(m_sync);
    return (m_data.find(key) != m_data.end());
}

template<typename SyncPolicy>
void DataStore<SyncPolicy>::remove(std::string_view key)
{
    std::unique_lock<SyncPolicy> lock(m_sync);
    if (auto it = m_data.find(key); it != m_data.end())
    {
        m_data.erase(it);
    }
}

template<typename SyncPolicy>
void DataStore<SyncPolicy>::clear()
{
    std::unique_lock<SyncPolicy> lock(m_sync);
    m_data.clear();
}

template<typename SyncPolicy>
DataStore<SyncPolicy>::const_iterator DataStore<SyncPolicy>::find(std::string_view key) const
{
    return m_data.find(key);
}

template<typename SyncPolicy>
DataStore<SyncPolicy>::iterator DataStore<SyncPolicy>::begin()
{
    return m_data.begin();
}

template<typename SyncPolicy>
DataStore<SyncPolicy>::iterator DataStore<SyncPolicy>::end()
{
    return m_data.end();
}

template<typename SyncPolicy>
DataStore<SyncPolicy>::const_iterator DataStore<SyncPolicy>::begin() const
{
    return m_data.begin();
}

template<typename SyncPolicy>
DataStore<SyncPolicy>::const_iterator DataStore<SyncPolicy>::end() const
{
    return m_data.end();
}

template<typename SyncPolicy>
inline std::ostream& operator<<(std::ostream& os, const DataStore<SyncPolicy>& ds)
{
    auto lockedDs = ds.lockedView();
    for (const auto& [key, val] : lockedDs)
    {
        os << "\t{ " << key << ", " << getValue(val) << " } | Type: " << getType(val) << "\n";
    }

    return os;
}
