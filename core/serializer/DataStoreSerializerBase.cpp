#include "DataStoreSerializerBase.hpp"

DataStoreSerializerBase::DataStoreSerializerBase()
    : RELATIVE_PATH{std::filesystem::current_path()}
{}

DataStoreSerializerBase::DataStoreSerializerBase(std::filesystem::path path)
    : RELATIVE_PATH{path}
{}

std::filesystem::path DataStoreSerializerBase::resolvePath(std::string path)
{
    std::filesystem::path finalPath = RELATIVE_PATH;

    while (path.substr(0, 3) == "../")
    {
        finalPath = finalPath.parent_path();
        path = path.substr(3);
    }

    return finalPath.append(path);
}
