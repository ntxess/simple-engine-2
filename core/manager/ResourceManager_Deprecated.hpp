// #pragma once

// #include "util/DataStore.hpp"
// #include "util/Logger.hpp"
// #include "util/AnyToString.hpp"
// #include <Thor/Resources.hpp>
// #include <any>
// #include <filesystem>
// #include <string>
// #include <unordered_map>

// template<typename T>
// class ResourceManager
// {
// public:
//     ResourceManager() = default;
//     ResourceManager(const DataStore& dataStore, thor::Resources::KnownIdStrategy known = thor::Resources::Reuse);
//     T& operator[](const std::string& id);
//     const T& operator[](const std::string& id) const;

//     const T& get(const std::string& id) const;
//     bool load(const DataStore& dataStore, thor::Resources::KnownIdStrategy known = thor::Resources::Reuse);
//     bool load(const std::string& id, const std::string& filepath, thor::Resources::KnownIdStrategy known = thor::Resources::Reuse);
//     bool unload();
//     bool unload(const std::string& id);

// private:
//     thor::ResourceHolder<T, std::string> m_holder;
// };

// template<typename T>
// inline ResourceManager<T>::ResourceManager(const DataStore& dataStore, thor::Resources::KnownIdStrategy known)
// {
//     load(dataStore, known);
// }

// template<typename T>
// inline T& ResourceManager<T>::operator[](const std::string& id)
// {
//     return m_holder[id];
// }

// template<typename T>
// inline const T& ResourceManager<T>::operator[](const std::string& id) const
// {
//     return m_holder[id];
// }

// template<typename T>
// inline const T& ResourceManager<T>::get(const std::string& id) const
// {
//     return m_holder[id];
// }

// template<typename T>
// inline bool ResourceManager<T>::load(const DataStore& dataStore, thor::Resources::KnownIdStrategy known)
// {
//     const std::string cwd = std::filesystem::current_path().string();
//     for (const auto& [id, path] : dataStore)
//     {
//         try
//         {
//             m_holder.acquire
//             (
//                 id,
//                 thor::Resources::fromFile<T>(cwd + getValue(path)),
//                 known
//             );
//         }
//         catch (const thor::ResourceLoadingException& e)
//         {
//             LOG_ERROR(Logger::get()) << "Failed to aquire resource at: " << cwd + getValue(path) << ". Error: " << e.what();
//             return false;
//         }
//     }
//     return true;
// }

// template<typename T>
// inline bool ResourceManager<T>::load(const std::string& id, const std::string& filepath, thor::Resources::KnownIdStrategy known)
// {
//     const std::string cwd = std::filesystem::current_path().string();
//     try
//     {
//         m_holder.acquire
//         (
//             id,
//             thor::Resources::fromFile<T>(cwd + filepath),
//             known
//         );
//     }
//     catch (const thor::ResourceLoadingException& e)
//     {
//         LOG_ERROR(Logger::get()) << "Failed to aquire resource at: " << cwd + filepath << ". Error: " << e.what();
//         return false;
//     }
//     return true;
// }

// template<typename T>
// inline bool ResourceManager<T>::unload()
// {
//     try
//     {
//         m_holder = thor::ResourceHolder<T, std::string>();
//     }
//     catch (thor::ResourceLoadingException& e)
//     {
//         LOG_ERROR(Logger::get()) << "Failed to release all resource. Error:" << e.what();
//         return false;
//     }
//     return true;
// }

// template<typename T>
// inline bool ResourceManager<T>::unload(const std::string& id)
// {
//     try
//     {
//         m_holder.release(id);
//     }
//     catch (thor::ResourceLoadingException& e)
//     {
//         LOG_ERROR(Logger::get()) << "Failed to release resource id: " << id << ". Error:" << e.what();
//         return false;
//     }
//     return true;
// }
