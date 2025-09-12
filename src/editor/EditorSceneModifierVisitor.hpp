#pragma once

#include "../core/interface/ISceneVisitor.hpp"
#include "../scene/Scenes.hpp"
#include "imgui.h"
#include "imgui-SFML.h"

class EditorSceneModifierVisitor : public ISceneVisitor
{
public:
    void visit(Sandbox* sandbox, entt::entity entityID) override;
    void visit(MainMenu* mainMenu, entt::entity entityID) override;
    void visit(GameOfLifeSim* gameOfLifeSim, entt::entity entityID) override;
    void visit(Editor* editor, entt::entity entityID) override;
};