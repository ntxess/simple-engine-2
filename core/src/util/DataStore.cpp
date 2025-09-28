#include "DataStore.hpp"

bool DataStore::contains(std::string_view key) const
{
    return (m_data.find(key) != m_data.end());
}

void DataStore::remove(std::string_view key)
{
    if (auto it = m_data.find(key); it != m_data.end())
    {
        m_data.erase(it);
    }
}

void DataStore::clear()
{
    m_data.clear();
}

DataStore::container_type& DataStore::data()
{
    return m_data;
}

const DataStore::container_type& DataStore::data() const
{
    return m_data;
}

DataStore::const_iterator DataStore::find(std::string_view key) const
{
    return m_data.find(key);
}

DataStore::iterator DataStore::begin()
{
    return m_data.begin();
}

DataStore::iterator DataStore::end()
{
    return m_data.end();
}

DataStore::const_iterator DataStore::begin() const
{
    return m_data.begin();
}

DataStore::const_iterator DataStore::end() const
{
    return m_data.end();
}
