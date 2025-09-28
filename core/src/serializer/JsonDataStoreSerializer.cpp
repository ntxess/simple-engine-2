#include "JsonDataStoreSerializer.hpp"

JsonDataStoreSerializer::JsonDataStoreSerializer()
    : DataStoreSerializerBase()
{}

JsonDataStoreSerializer::JsonDataStoreSerializer(std::filesystem::path path)
    : DataStoreSerializerBase(path)
{}

bool JsonDataStoreSerializer::load(std::string_view filename, DataStore& dataStore)
{
    const auto path = resolvePath(filename.data());
    std::ifstream ifs(path, std::fstream::app);

    if (!ifs.good())
    {
        LOG_ERROR(Logger::get()) << "File cannot be read: " << path.c_str();
        return false;
    }

    if (std::filesystem::is_empty(path))
    {
        LOG_ERROR(Logger::get()) << "File cannot be found: " << path.c_str();
        return false;
    }

    rapidjson::IStreamWrapper isw(ifs);
    rapidjson::Document doc;
    doc.ParseStream(isw);

    std::string key;
    read(key, doc, dataStore);

    return true;
}

bool JsonDataStoreSerializer::save(std::string_view filename, const DataStore& dataStore)
{
    const auto path = resolvePath(filename.data());
    std::ifstream ifs(path, std::fstream::app);

    if (!ifs.good())
    {
        LOG_ERROR(Logger::get()) << "File cannot be read: " << path.c_str();
        return false;
    }

    rapidjson::Document doc;
    doc.SetObject();
    write(doc, dataStore);

    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    std::string json(buffer.GetString(), buffer.GetSize());

    std::ofstream ofs(path);
    if (!ofs.good())
    {
        LOG_ERROR(Logger::get()) << "File cannot be written: " << path.c_str();
        return false;
    }

    ofs << json;

    return true;
}

bool JsonDataStoreSerializer::update(std::string_view filename, const DataStore& dataStore)
{
    const auto path = resolvePath(filename.data());
    std::ifstream ifs(path, std::fstream::app);

    if (!ifs.good())
    {
        LOG_ERROR(Logger::get()) << "File cannot be read: " << path.c_str();
        return false;
    }

    // Read in the json file
    rapidjson::IStreamWrapper isw(ifs);
    rapidjson::Document doc;
    doc.ParseStream(isw);

    // Update the json document with the new data
    if (doc.IsObject())
    {
        findAndReplace(doc, doc, dataStore);
    }
    else
    {
        LOG_ERROR(Logger::get()) << "Invalid JSON format in file: " << path.c_str();
        return false;
    }

    // Setup the document to write
    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    std::string json(buffer.GetString(), buffer.GetSize());

    std::ofstream ofs(path);
    if (!ofs.good())
    {
        LOG_ERROR(Logger::get()) << "File cannot be written: " << path.c_str();
        return false;
    }

    ofs << json;

    return true;
}

void JsonDataStoreSerializer::read(std::string_view key, rapidjson::Value& val, DataStore& dataStore)
{
    if (val.IsObject())
    {
        for (auto it = val.MemberBegin(); it != val.MemberEnd(); ++it)
        {
            read(it->name.GetString(), it->value, dataStore);
        }

        return;
    }

    if (val.IsArray())
    {
        std::vector<std::any> vec;
        for (rapidjson::SizeType i = 0; i < val.Size(); ++i)
        {
            if (val[i].IsArray())
            {
                vecParseHelper(key, val[i], vec);
            }
            else
            {
                auto data = valueToAny(val[i]);
                if (data.has_value())
                {
                    vec.push_back(data.value());
                }
                else
                {
                    LOG_ERROR(Logger::get()) << "Failed to convert value to any type for key: " << key;
                }
            }
        }

        if (!vec.empty())
        {
            dataStore.set(key, vec);
        }

        return;
    }

    auto data = valueToAny(val);
    if (data.has_value())
    {
        dataStore.set(key, data.value());
    }
    else
    {
        LOG_ERROR(Logger::get()) << "Failed to convert value to any type for key: " << key;
    }
}

void JsonDataStoreSerializer::write(rapidjson::Document& doc, const DataStore& dataStore)
{
    for (const auto& [key, data] : dataStore)
    {
        auto val = createJsonValue(doc, data);
        if (!val.has_value())
        {
            LOG_ERROR(Logger::get()) << "Failed to convert value to any type for key: " << key;
            continue;
        }

        rapidjson::Value index(key.c_str(), static_cast<rapidjson::SizeType>(key.size()), doc.GetAllocator());
        doc.AddMember(index, val.value(), doc.GetAllocator());
    }
}

void JsonDataStoreSerializer::findAndReplace(rapidjson::Document& doc, rapidjson::Value& val, const DataStore& dataStore)
{
    if (val.IsObject())
    {
        for (auto itr = val.MemberBegin(); itr != val.MemberEnd(); ++itr)
        {
            std::string keyStr = itr->name.GetString();

            // If this key is in the updates map, replace it
            if (auto it = dataStore.find(keyStr); it != dataStore.end())
            {
                itr->value = createJsonValue(doc, it->second).value_or(rapidjson::Value{});

                // If the value is an object, we need to replace it
                if (itr->value.IsObject())
                {
                    itr->value = createJsonValue(doc, it->second).value_or(rapidjson::Value{});
                }
                else
                {
                    LOG_ERROR(Logger::get()) << "Failed to convert value to any type for key: " << keyStr;
                }
            }

            // Recurse deeper if the value is also an object
            findAndReplace(doc, itr->value, dataStore);
        }
    }
    else if (val.IsArray())
    {
        for (auto& element : val.GetArray())
        {
            findAndReplace(doc, element, dataStore);
        }
    }
}

std::optional<std::any> JsonDataStoreSerializer::valueToAny(const rapidjson::Value& val)
{
    switch (val.GetType())
    {
    case rapidjson::kStringType:
    {
        auto str = val.GetString();
        return std::string(val.GetString());
    }

    case rapidjson::kNumberType:
        if (val.IsInt())
            return val.GetInt();
        else if (val.IsDouble())
            return val.GetDouble();
        else return std::string();

    case rapidjson::kTrueType:
        [[fallthrough]];

    case rapidjson::kFalseType:
        return val.GetBool();

    case rapidjson::kArrayType:
        return val.GetArray();
    }
    return std::nullopt;
}

std::optional<rapidjson::Value> JsonDataStoreSerializer::createJsonValue(rapidjson::Document& doc, const std::any& data)
{
    rapidjson::Value val;
    if (data.type() == typeid(const char*))
    {
        val.SetString(std::any_cast<const char*>(data), doc.GetAllocator());
    }
    else if (data.type() == typeid(std::string))
    {
        val.SetString(std::any_cast<std::string>(data).c_str(), doc.GetAllocator());
    }
    else if (data.type() == typeid(bool))
    {
        val.SetBool(std::any_cast<bool>(data));
    }
    else if (data.type() == typeid(int))
    {
        val.SetInt(std::any_cast<int>(data));
    }
    else if (data.type() == typeid(float))
    {
        val.SetFloat(std::any_cast<float>(data));
    }
    else if (data.type() == typeid(double))
    {
        val.SetDouble(std::any_cast<double>(data));
    }
    else if (data.type() == typeid(std::vector<std::any>))
    {
        val.SetArray();
        for (const auto& item : std::any_cast<std::vector<std::any>>(data))
        {
            auto itemVal = createJsonValue(doc, item).value_or(rapidjson::Value{});
            val.PushBack(itemVal, doc.GetAllocator());
        }
    }

    return val;
}

void JsonDataStoreSerializer::vecParseHelper(std::string_view key, rapidjson::Value& val, std::vector<std::any>& vec)
{
    std::vector<std::any> newVec;
    for (rapidjson::SizeType i = 0; i < val.Size(); ++i)
    {
        if (val[i].IsArray())
        {
            vecParseHelper(key, val[i], vec);
        }
        else
        {
            auto data = valueToAny(val[i]);
            if (data.has_value())
            {
                newVec.push_back(data.value());
            }
            else
            {
                LOG_ERROR(Logger::get()) << "Failed to convert value to any type for key: " << key;
            }
        }
    }
    vec.push_back(newVec);
}
