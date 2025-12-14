#pragma once

#include <any>
#include <mutex>
#include <ostream>
#include <optional>
#include <string>
#include <string_view>
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
