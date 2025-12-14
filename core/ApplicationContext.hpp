#pragma once

#include "manager/ResourceManager.hpp"
#include "manager/SceneManager.hpp"
#include "serializer/JsonDataStoreSerializer.hpp"
#include "serializer/GameDataStoreSerializer.hpp"
#include "util/DataStore.hpp"
#include "util/EventBus.hpp"
#include <SFML/System/Clock.hpp>
#include <SFML/Graphics/RenderWindow.hpp>
#include <SFML/Graphics/View.hpp>
#include <SFML/Graphics/Texture.hpp>
#include <SFML/Audio/SoundBuffer.hpp>
#include <any>
#include <memory>
#include <unordered_map>

constexpr size_t numEventThread = 2;
constexpr size_t numQueueCapacity = 200;

struct ApplicationContext
{
	sf::Clock clock;
	sf::RenderWindow window;
	sf::View viewport;
	float aspectRatio;
	float deltaTime;
	DataStore<> configData;
	DataStore<> gameData;
	JsonDataStoreSerializer configDataSerializer;
	GameDataStoreSerializer gameDataSerializer;
	ResourceManager<sf::Texture, MutexSync> textureManager;
	ResourceManager<sf::SoundBuffer, MutexSync> audioManager;
	SceneManager sceneManager;
	EventBus eventBus{numEventThread, numQueueCapacity};
};