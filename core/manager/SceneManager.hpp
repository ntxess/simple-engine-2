#pragma once

#include "interface/IScene.hpp"
#include <memory>
#include <stack>

struct ApplicationContext;

class SceneManager
{
public:
    SceneManager();

    void addScene(std::unique_ptr<IScene> newScene, bool isReplacing = true, ApplicationContext* sysData = nullptr);

    void removeScene();
    void processChange();
    IScene* getActiveScene();

private:
    std::stack<std::unique_ptr<IScene>> m_scenes;
    std::unique_ptr<IScene> m_newScene;
    bool m_removeFlag;
    bool m_addFlag;
    bool m_replaceFlag;
};