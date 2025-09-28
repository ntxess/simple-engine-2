#pragma once

#include <any>
#include <string>
#include <vector>

inline std::string getType(const std::any& data)
{
    if (std::any_cast<std::string>(&data))
        return "string";
    else if (std::any_cast<const char*>(&data))
        return "cstring";
    else if (std::any_cast<bool>(&data))
        return "bool";
    else if (std::any_cast<int>(&data))
        return "int";
    else if (std::any_cast<float>(&data))
        return "float";
    else if (std::any_cast<double>(&data))
        return "double";
    else if (auto x = std::any_cast<std::vector<std::any>>(&data))
    {
        std::string result = "<";
        for (size_t i = 0; i < (*x).size(); ++i)
        {
            result += getType((*x)[i]);
            if (i + 1 < (*x).size())
                result += ", ";
        }
        result.push_back('>');
        return result;
    }
    return "Undefined Type";
}

inline std::string getValue(const std::any& data)
{
    if (data.has_value())
    {
        try
        {
            if (auto x = std::any_cast<std::string>(&data))
                return *x;
            else if (auto x = std::any_cast<const char*>(&data))
                return *x;
            else if (auto x = std::any_cast<bool>(&data))
                return (std::to_string(*x) == "1") ? "true" : "false";
            else if (auto x = std::any_cast<int>(&data))
                return std::to_string(*x);
            else if (auto x = std::any_cast<float>(&data))
                return std::to_string(*x);
            else if (auto x = std::any_cast<double>(&data))
                return std::to_string(*x);
            else if (auto x = std::any_cast<std::vector<std::any>>(&data))
            {
                std::string result = "[";
                for (size_t i = 0; i < (*x).size(); ++i)
                {
                    result += getValue((*x)[i]);
                    if (i + 1 < (*x).size())
                        result += ", ";
                }
                result.push_back(']');
                return result;
            }
        }
        catch (const std::bad_any_cast& e)
        {
            LOG_ERROR(Logger::get()) << "Failed to cast value. Error: " << e.what();
            return std::string{};
        }
    }
    return std::string{};
}