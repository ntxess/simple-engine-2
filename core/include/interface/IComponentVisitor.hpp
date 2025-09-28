#pragma once

#include "entt/entity/entity.hpp"

class Effects;
class EffectsList;
class EntityStatus;
class Hitbox;
class MovementPattern;
class PlayerInput;
class SceneViewRenderer;
class Sprite;
class StatusModEvent;
class TeamTag;
class UpdateEntityEvent;
class UpdateEntityPolling;

class IComponentVisitor
{
public:
    virtual ~IComponentVisitor() = default;
    virtual void visit(Effects* effects, entt::entity entityID) = 0;
    virtual void visit(EffectsList* effectsList, entt::entity entityID) = 0;
    virtual void visit(EntityStatus* entityStatus, entt::entity entityID) = 0;
    virtual void visit(Hitbox* hitbox, entt::entity entityID) = 0;
    virtual void visit(MovementPattern* movementPattern, entt::entity entityID) = 0;
    virtual void visit(PlayerInput* playerInput, entt::entity entityID) = 0;
    virtual void visit(SceneViewRenderer* sceneViewRenderer, entt::entity entityID) = 0;
    virtual void visit(Sprite* sprite, entt::entity entityID) = 0;
    virtual void visit(StatusModEvent* statusModEvent, entt::entity entityID) = 0;
    virtual void visit(TeamTag* teamTag, entt::entity entityID) = 0;
    virtual void visit(UpdateEntityEvent* updateEntityEvent, entt::entity entityID) = 0;
    virtual void visit(UpdateEntityPolling* updateEntityPolling, entt::entity entityID) = 0;
};