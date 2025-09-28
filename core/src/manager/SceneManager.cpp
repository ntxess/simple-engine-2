#include "SceneManager.hpp"

/**
 * @brief [Public] Default constructor.
*/
SceneManager::SceneManager()
    : m_removeFlag(false), m_addFlag(false), m_replaceFlag(false)
{}

/**
 * @brief [Public] Adds a new scene. New scene is ready upon the next processChange() call.
 * If the new scene is replacing, destory the current scene. Otherwise, pause the current scene.
 * Lazy-init for global data if missed by constructor.
 * @param newScene
 * @param isReplacing
*/
void SceneManager::addScene(std::unique_ptr<IScene> newScene, bool isReplacing, ApplicationContext* data)
{
    m_addFlag = true;
    m_replaceFlag = isReplacing;
    if (data) newScene->setApplicationContext(data);
    m_newScene = std::move(newScene);
}

/**
 * @brief [Public] Remove the current scene. Scene is removed upon the next processChange() call.
 * Scene cannot be removed if it is the only scene available.
*/
void SceneManager::removeScene()
{
    m_removeFlag = true;
}

/**
 * @brief [Public] Process the new scene changes. Adding new scenes will push new scene to top of the stack.
 * Removing scene will pop current scene off stack.
*/
void SceneManager::processChange()
{
    if (m_removeFlag && !m_scenes.empty())
    {
        m_scenes.pop();
        if (!m_scenes.empty())
        {
            m_scenes.top()->resume();
        }
        m_removeFlag = false;
    }

    if (m_addFlag)
    {
        if (!m_scenes.empty())
        {
            if (m_replaceFlag)
            {
                m_scenes.pop();
            }
            else
            {
                m_scenes.top()->pause();
            }
        }
        m_scenes.push(std::move(m_newScene));
        m_scenes.top()->init();
        m_addFlag = false;
    }
}

/**
 * @brief [Public] Get current scene (Top scene of the stack).
 * @return Current scene pointer
*/
IScene* SceneManager::getActiveScene()
{
    return m_scenes.top().get();
}