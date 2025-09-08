#pragma once

#include "../core/interface/IScene.hpp"
#include "../core/component/SceneViewRenderer.hpp"
#include "entt/entity/entity.hpp"
#include <memory>

class EditorSceneAdapter
{
public:
    EditorSceneAdapter() = delete;
    EditorSceneAdapter(std::unique_ptr<IScene> scn, unsigned int width, unsigned int height, const sf::ContextSettings& settings);

    void processInput();
    void processEvent(const sf::Event& event);
    void render();
    void update();
    entt::registry& getRegistry() const;
    sf::RenderTexture& getRenderTexture() const;
    IScene* get() const;

private:
    entt::entity m_renderTextureID;
    std::unique_ptr<IScene> m_scene;
};

