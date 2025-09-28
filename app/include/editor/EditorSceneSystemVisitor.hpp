#pragma once

#include "interface/ISceneVisitor.hpp"
#include "scene/Scenes.hpp"
#include "entt/entity/entity.hpp"
#include "imgui.h"
#include "imgui-SFML.h"

class EditorSceneSystemVisitor : public ISceneVisitor
{
public:
    void visit(Sandbox* sandbox, entt::entity entityID) override;
    void visit(MainMenu* mainMenu, entt::entity entityID) override;
    void visit(GameOfLifeSim* gameOfLifeSim, entt::entity entityID) override;
    void visit(Editor* editor, entt::entity entityID) override;
};