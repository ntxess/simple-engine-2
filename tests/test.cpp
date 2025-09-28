#include "pch.h"
#include "../src/core/util/DataStore.hpp"
#include "../src/core/serializer/JsonDataStoreSerializer.hpp"

namespace RampageTests::Serializer
{
    TEST(JsonDataStoreSerializerTest, TestLoad)
    {
        JsonDataStoreSerializer serializer;
        DataStore dataStore;
        EXPECT_TRUE(serializer.load("../../../config/default_config.json", dataStore));
        EXPECT_EQ(dataStore.get<std::string>("name").value_or(""), "unittest-defaultConfig");
        EXPECT_EQ(dataStore.get<int>("width").value_or(0), 1920);
        EXPECT_EQ(dataStore.get<int>("height").value_or(0), 1080);
    }

    TEST(JsonDataStoreSerializerTest, TestSave)
    {
        JsonDataStoreSerializer serializer;
        DataStore saveDataStore;
        DataStore loadDataStore;

        saveDataStore.set("name", std::string("unittest-defaultConfig"));
        saveDataStore.set("width", 1920);
        saveDataStore.set("height", 1080);
        saveDataStore.set("frame-rate", 160.0);
        saveDataStore.set("array", std::vector<std::any>{ 1, 2, 3, 4, 5 });

        EXPECT_TRUE(serializer.save("../../../config/save_config.json", saveDataStore));
        EXPECT_TRUE(serializer.load("../../../config/default_config.json", loadDataStore));
        EXPECT_EQ(saveDataStore.get<std::string>("name").value_or("save"), loadDataStore.get<std::string>("name").value_or("load"));
        EXPECT_EQ(saveDataStore.get<int>("width").value_or(1), loadDataStore.get<int>("width").value_or(2));
        EXPECT_EQ(saveDataStore.get<int>("height").value_or(1), loadDataStore.get<int>("height").value_or(2));
        EXPECT_EQ(saveDataStore.get<double>("frame-rate").value_or(1.0f), loadDataStore.get<double>("frame-rate").value_or(2.0f));

        const auto savedArray = saveDataStore.get<std::vector<std::any>>("array").value();
        const auto loadedArray = loadDataStore.get<std::vector<std::any>>("array").value();
        for (size_t i = 0; i < savedArray.size(); ++i) 
        {
            EXPECT_EQ(std::any_cast<int>(savedArray[i]), std::any_cast<int>(loadedArray[i]));
        }
    }

    TEST(JsonDataStoreSerializerTest, TestUpdate)
    {
        const std::string defaultFile = "../../../config/default_config.json";
        const std::string updatedFile = "../../../config/updated_config.json";

        JsonDataStoreSerializer serializer;
        DataStore dataStore;

        EXPECT_TRUE(serializer.load(defaultFile, dataStore));

        std::ifstream f1(defaultFile);
        std::ifstream f2(updatedFile);

        std::stringstream original, preUpdated, postUpdated, reverted;
        original << f1.rdbuf();
        preUpdated << f2.rdbuf();

        f1.close();
        f2.close();

        EXPECT_TRUE(original.str() == preUpdated.str());

        dataStore.set("name", std::string("unittest-updatedConfig"));
        dataStore.set("width", 1280);
        dataStore.set("height", 720);
        dataStore.set("frame-rate", 60.0);
        dataStore.set("array", std::vector<std::any>{ 6, 7, 8, 9, 10 });

        EXPECT_TRUE(serializer.update(updatedFile, dataStore));
        postUpdated << f2.rdbuf();
        f2.close();

        EXPECT_TRUE(original.str() != postUpdated.str());

        dataStore.set("name", std::string("unittest-defaultConfig"));
        dataStore.set("width", 1920);
        dataStore.set("height", 1080);
        dataStore.set("frame-rate", 160.0);
        dataStore.set("array", std::vector<std::any>{ 1, 2, 3, 4, 5 });

        EXPECT_TRUE(serializer.update(updatedFile, dataStore));

        f2.open(updatedFile);
        reverted << f2.rdbuf();
        f2.close();

        EXPECT_TRUE(original.str() == reverted.str());
    }

    TEST(JsonDataStoreSerializerTest, TestInvalidFile)
    {
        JsonDataStoreSerializer serializer;
        DataStore dataStore;
        EXPECT_FALSE(serializer.load("../../../config/invalid_config.json", dataStore));
    }
}