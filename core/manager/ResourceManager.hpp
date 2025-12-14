#pragma once

#include <any>
#include <expected>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "util/AnyToString.hpp"
#include "util/Logger.hpp"
#include "util/SyncPolicy.hpp"
#include "util/TransparentStringHash.hpp"

// Maintained Thor's resource manager identity to keep existing code unchaged
template<typename T, typename SyncPolicy>
class ResourceManager
{
public:
    enum class ManagementStrategy
    {
        AssumeNew,
        Reuse,
        Reload
    };

public:
    using key_type = std::string;
    using mapped_type = T;
    using container_type = std::unordered_map<key_type, mapped_type, TransparentStringHash, TransparentStringEqual>;
    using iterator = container_type::iterator;
    using const_iterator = container_type::const_iterator;
    
    ResourceManager() = default;
    ~ResourceManager() = default;
    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    std::expected<mapped_type, bool> get(std::string_view key) const;
    bool load(std::string_view key, std::string_view filepath, ResourceManager::ManagementStrategy known = ResourceManager::ManagementStrategy::Reuse);
    void unload(std::string_view key);
    void unloadAll();

private:
    mutable SyncPolicy m_sync{};
    container_type m_holder;
};

template <typename T, typename SyncPolicy>
inline std::expected<T, bool> ResourceManager<T, SyncPolicy>::get(std::string_view key) const
{
    std::unique_lock<SyncPolicy> lock(m_sync);
    auto it = m_holder.find(key);
    if (it != m_holder.end())
        return it->second;
    return std::unexpected(false);
}

template <typename T, typename SyncPolicy>
inline bool ResourceManager<T, SyncPolicy>::load(std::string_view key, std::string_view filepath, ResourceManager::ManagementStrategy known)
{
    auto path = std::filesystem::current_path() / filepath;
    T resource;
    if (!resource.loadFromFile(path))
    {
        LOG_ERROR(Logger::get()) << "Failed to load resource: " << path;
        return false;
    }

    std::unique_lock<SyncPolicy> lock(m_sync);
    if (known == ResourceManager::ManagementStrategy::AssumeNew)
    {
        // If the resouce already exists somewhere 
        // do not store and return failed to load (false).
        if (m_holder.find(key) != m_holder.end())
        {
            LOG_ERROR(Logger::get()) << "Resource with key already exists: " << key;
            return false;
        }
        m_holder[std::string(key)] = resource;
        return true;
    }
    else if (known == ResourceManager::ManagementStrategy::Reuse)
    {
        // If resource is found, reuse the resource 
        // and return true for consistency.
        if (m_holder.find(key) == m_holder.end())
        {
            m_holder[std::string(key)] = resource;
        }
        return true;
    }
    else if (known == ResourceManager::ManagementStrategy::Reload)
    {
        // Always load new resource.
        // Resource does not need to be manually cleanup.
        m_holder[std::string(key)] = resource;
        return true;
    }
    return false;
}

template <typename T, typename SyncPolicy>
inline void ResourceManager<T, SyncPolicy>::unload(std::string_view key)
{
    std::unique_lock<SyncPolicy> lock(m_sync);
    if (m_holder.find(key) != m_holder.end())
    {
        m_holder.erase(key);
    }
}

template <typename T, typename SyncPolicy>
inline void ResourceManager<T, SyncPolicy>::unloadAll()
{
    std::unique_lock<SyncPolicy> lock(m_sync);
    m_holder.clear();
}