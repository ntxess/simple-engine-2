#include "EditorSceneModifierVisitor.hpp"

void EditorSceneModifierVisitor::visit(Sandbox* sandbox, entt::entity entityID)
{
	sandbox->getSystemManager()->getSystem<CollisionSystem>()->remove(sandbox->getRegistry(), entityID);
}

void EditorSceneModifierVisitor::visit(MainMenu* mainMenu, entt::entity entityID)
{}

void EditorSceneModifierVisitor::visit(GameOfLifeSim* gameOfLifeSim, entt::entity entityID)
{}

void EditorSceneModifierVisitor::visit(Editor* editor, entt::entity entityID)
{}