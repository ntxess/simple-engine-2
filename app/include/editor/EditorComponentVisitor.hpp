#pragma once

#include "Components.hpp"
#include "interface/IComponentVisitor.hpp"
#include "imgui.h"
#include "imgui-SFML.h"
#include "entt/entity/entity.hpp"

class EditorComponentVisitor : public IComponentVisitor
{
public:
    void visit(Effects* effects, entt::entity entityID) override;
    void visit(EffectsList* effectsList, entt::entity entityID) override;
    void visit(EntityStatus* entityStatus, entt::entity entityID) override;
    void visit(Hitbox* hitbox, entt::entity entityID) override;
    void visit(MovementPattern* movementPattern, entt::entity entityID) override;
    void visit(PlayerInput* playerInput, entt::entity entityID) override;
    void visit(SceneViewRenderer* sceneViewRenderer, entt::entity entityID) override;
    void visit(Sprite* sprite, entt::entity entityID) override;
    void visit(StatusModEvent* statusModEvent, entt::entity entityID) override;
    void visit(TeamTag* teamTag, entt::entity entityID) override;
    void visit(UpdateEntityEvent* updateEntityEvent, entt::entity entityID) override;
    void visit(UpdateEntityPolling* updateEntityPolling, entt::entity entityID) override;
};