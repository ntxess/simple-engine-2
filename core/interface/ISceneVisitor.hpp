#pragma once

#include <entt/entity/entity.hpp>

class Sandbox;
class MainMenu;
class GameOfLifeSim;
class Editor;

class ISceneVisitor
{
public:
    virtual ~ISceneVisitor() = default;
    virtual void visit(Sandbox* sandbox, entt::entity entityID) = 0;
    virtual void visit(MainMenu* mainMenu, entt::entity entityID) = 0;
    virtual void visit(GameOfLifeSim* gameOfLifeSim, entt::entity entityID) = 0;
    virtual void visit(Editor* editor, entt::entity entityID) = 0;
};