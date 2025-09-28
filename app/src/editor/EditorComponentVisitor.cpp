#include "EditorComponentVisitor.hpp"

void EditorComponentVisitor::visit(Effects* effects, entt::entity entityID)
{
    // Implemented in the EffectsList visitor
}

void EditorComponentVisitor::visit(EffectsList* effectsList, entt::entity entityID)
{
    ImGui::NewLine();

    const std::string ID = std::to_string(static_cast<unsigned int>(entityID));
    const float divider1Pos = ImGui::GetWindowWidth() / 3.f;
    const float elDivider1Pos = ImGui::GetWindowWidth() / 6.f;
    const char* effectTypes[] = { "NULLTYPE", "INSTANT", "OVERTIME", "TEMPTIMED" }; // NOTE: Update this when adding new effect types

    ImGui::PushItemWidth(elDivider1Pos);

    ImGui::Text("Effect Type");
    ImGui::SameLine(elDivider1Pos + 10.f);
    ImGui::Text("Aff. Status");
    ImGui::SameLine(elDivider1Pos * 2 + 20.f);
    ImGui::Text("Modifier");
    ImGui::SameLine(elDivider1Pos * 3 + 30.f);
    ImGui::Text("Max Duration");
    ImGui::SameLine(elDivider1Pos * 4 + 40.f);
    ImGui::Text("Tick Rate");

    int index = 0;
    int discardIndex = -1;
    for (auto& [effectType, effect] : effectsList->effectsList)
    {
        int selectedEffectType = static_cast<int>(effectType);
        if (ImGui::Combo(("##EffectType" + std::to_string(index) + ID).c_str(), &selectedEffectType, effectTypes, IM_ARRAYSIZE(effectTypes)))
            effectType = static_cast<EffectType>(selectedEffectType);

        ImGui::SameLine(elDivider1Pos + 10.f);

        char* targetStat = const_cast<char*>(effect.targetStat.c_str());
        if (ImGui::InputText(("##EffectName" + std::to_string(index) + ID).c_str(), targetStat, 64, 0))
            effect.targetStat = targetStat;

        ImGui::SameLine(elDivider1Pos * 2 + 20.f);

        ImGui::InputFloat(("##EffectVal" + std::to_string(index) + ID).c_str(), &effect.modificationVal);

        ImGui::SameLine(elDivider1Pos * 3 + 30.f);

        int maxDuration = static_cast<int>(effect.maxDuration.count());
        if (ImGui::InputScalar(("##EffectMaxDuration" + std::to_string(index) + ID).c_str(), ImGuiDataType_S32, &maxDuration))
            effect.maxDuration = std::chrono::milliseconds(maxDuration);

        ImGui::SameLine(elDivider1Pos * 4 + 40.f);

        int tickRate = static_cast<int>(effect.tickRate.count());
        if (ImGui::InputScalar(("##EffectTickRate" + std::to_string(index) + ID).c_str(), ImGuiDataType_S32, &tickRate))
            effect.tickRate = std::chrono::milliseconds(tickRate);

        ImGui::SameLine(elDivider1Pos * 5 + 50.f);

        if (ImGui::Button(("-##DiscardEffectButton" + std::to_string(index) + ID).c_str(), { 20.f, 20.f }))
            discardIndex = index;

        ++index;
    }

    if (discardIndex != -1)
    {
        effectsList->effectsList.erase(effectsList->effectsList.begin() + discardIndex);
    }

    ImGui::NewLine();
    ImGui::Text("Add New Effects");
    ImGui::Text("Effect Type");
    ImGui::SameLine(elDivider1Pos + 10.f);
    ImGui::Text("Aff. Status");
    ImGui::SameLine(elDivider1Pos * 2 + 20.f);
    ImGui::Text("Modifier");
    ImGui::SameLine(elDivider1Pos * 3 + 30.f);
    ImGui::Text("Max Duration");
    ImGui::SameLine(elDivider1Pos * 4 + 40.f);
    ImGui::Text("Tick Rate");

    static int newEffectType = 0;
    static char newStatusToModify[64] = { 0 };
    static float newEffectVal = 0.f;
    static int newMaxDuration = 0;
    static int newTickRate = 0;

    ImGui::Combo(("##NewEffectType" + std::to_string(index) + ID).c_str(), &newEffectType, effectTypes, IM_ARRAYSIZE(effectTypes));
    ImGui::SameLine(elDivider1Pos + 10.f);
    ImGui::InputText(("##NewEffectName" + std::to_string(index) + ID).c_str(), newStatusToModify, 64, 0);
    ImGui::SameLine(elDivider1Pos * 2 + 20.f);
    ImGui::InputFloat(("##NewEffectVal" + std::to_string(index) + ID).c_str(), &newEffectVal);
    ImGui::SameLine(elDivider1Pos * 3 + 30.f);
    ImGui::InputInt(("##NewEffectMaxDuration" + std::to_string(index) + ID).c_str(), &newMaxDuration);
    ImGui::SameLine(elDivider1Pos * 4 + 40.f);
    ImGui::InputInt(("##NewEffectTickRate" + std::to_string(index) + ID).c_str(), &newTickRate);
    ImGui::SameLine(elDivider1Pos * 5 + 50.f);
    if (ImGui::Button(("+##AddEffectButton" + std::to_string(index) + ID).c_str(), { 20.f, 20.f }))
    {
        if (newStatusToModify[0] != '\0')
        {
            effectsList->effectsList.push_back({
                static_cast<EffectType>(newEffectType),
                Effects{newStatusToModify,
                    newEffectVal,
                    std::chrono::milliseconds(newMaxDuration),
                    std::chrono::milliseconds(newTickRate
                )}
                });

            newStatusToModify[0] = '\0';
            newEffectVal = 0.f;
            newMaxDuration = 0;
            newTickRate = 0;
        }
    }
    ImGui::PopItemWidth();

    ImGui::NewLine();
}

void EditorComponentVisitor::visit(EntityStatus* entityStatus, entt::entity entityID)
{
    ImGui::NewLine();

    const std::string ID = std::to_string(static_cast<unsigned int>(entityID));
    const float divider1Pos = ImGui::GetWindowWidth() / 3.f;

    ImGui::Text("Status");
    ImGui::SameLine(divider1Pos);
    ImGui::Text("Value");

    std::string discardedStat = "";
    for (auto& [name, val] : entityStatus->values)
    {
        ImGui::Text(name.c_str());
        ImGui::SameLine(divider1Pos);
        ImGui::SetNextItemWidth(ImGui::GetWindowWidth() / 6.f);
        ImGui::InputFloat(("##StatValInput" + name + ID).c_str(), &val);
        ImGui::SameLine();
        if (ImGui::Button(("-##DiscardStatButton" + name + ID).c_str(), { 20.f, 20.f }))
            discardedStat = name;
    }

    if (!discardedStat.empty())
    {
        entityStatus->values.erase(discardedStat);
    }

    ImGui::NewLine();
    ImGui::Text("Add New Stat");
    ImGui::Text("Status");
    ImGui::SameLine(divider1Pos);
    ImGui::Text("Value");
    static char newStatName[64] = { 0 };
    static float newStatVal = 0.f;
    ImGui::SetNextItemWidth(ImGui::GetWindowWidth() / 6.f);
    ImGui::InputText(("##NewStatName" + ID).c_str(), newStatName, 64);
    ImGui::SameLine(divider1Pos);
    ImGui::SetNextItemWidth(ImGui::GetWindowWidth() / 6.f);
    ImGui::InputFloat(("##NewStatVal" + ID).c_str(), &newStatVal);
    ImGui::SameLine();
    if (ImGui::Button(("+##AddStatButton" + ID).c_str(), { 20.f, 20.f }))
    {
        if (newStatName[0] != '\0')
        {
            entityStatus->values[newStatName] = newStatVal;
            newStatName[0] = '\0';
            newStatVal = 0.f;
        }
    }

    ImGui::NewLine();
}

void EditorComponentVisitor::visit(Hitbox* hitbox, entt::entity entityID)
{
}

void EditorComponentVisitor::visit(MovementPattern* movementPattern, entt::entity entityID)
{
    ImGui::NewLine();

    const std::string ID = std::to_string(static_cast<unsigned int>(entityID));
    const float ratio = ImGui::GetWindowWidth() / ImGui::GetWindowHeight();

    ImGui::Text("Way Point");
    ImGui::SameLine(ImGui::GetWindowWidth() / 3.f);
    ImGui::Text("Distance To Next");
    ImGui::SameLine(ImGui::GetWindowWidth() * 4.f / 6.f);
    ImGui::Text("Total Distance");

    ImGui::PushItemWidth(ImGui::GetWindowWidth() / 4.f);

    size_t waypointNum = 0;
    WayPoint* current = movementPattern->movePattern.get();
    while (current != nullptr)
    {
        std::string wpNumText = std::to_string(waypointNum);
        if (current->distanceTotal < movementPattern->distance && current->nextWP && movementPattern->distance < current->nextWP->distanceTotal)
        {
            ImGui::TextColored(ImVec4(0.f, 1.f, 0.f, 1.f), (wpNumText + "  Run ").c_str());
            ImGui::SameLine(ImGui::GetWindowWidth() / 8.f);
            ImGui::TextColored(ImVec4(0.f, 1.f, 0.f, 1.f), std::to_string(movementPattern->distance - current->distanceTotal).c_str());
        }
        else
        {
            ImGui::Text(wpNumText.c_str());
        }

        ImGui::SameLine(ImGui::GetWindowWidth() / 3.f);
        ImGui::InputFloat(("##DistanceToNext" + wpNumText + ID).c_str(), &current->distanceToNext);
        ImGui::SameLine(ImGui::GetWindowWidth() * 4.f / 6.f);
        ImGui::InputFloat(("##DistanceTotal" + wpNumText + ID).c_str(), &current->distanceTotal);

        current = current->next();
        ++waypointNum;
    }

    ImGui::Checkbox(("Repeat##" + ID).c_str(), &movementPattern->repeat);

    ImGui::PopItemWidth();

    ImGui::NewLine();
}

void EditorComponentVisitor::visit(PlayerInput* playerInput, entt::entity entityID)
{

}

void EditorComponentVisitor::visit(SceneViewRenderer* sceneViewRenderer, entt::entity entityID)
{
    // No implementation
}

void EditorComponentVisitor::visit(Sprite* sprite, entt::entity entityID)
{
    ImGui::NewLine();

    const std::string ID = std::to_string(static_cast<unsigned int>(entityID));
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const float divider1Pos = size.x / 3.f;
    const float divider2Pos = size.x * 2.f / 3.f;

    // Standardize all widgets to a 75% divided sections width
    ImGui::PushItemWidth(divider1Pos * 0.75f);

    ImGui::SeparatorText("Texture");

    float xOrigin = sprite->getOrigin().x;
    float yOrigin = sprite->getOrigin().y;
    int xTextureWidth = sprite->getTextureRect().width;
    int yTextureHeight = sprite->getTextureRect().height;
    bool isRepeated = sprite->getTexture()->isRepeated();
    int rHexVal = sprite->getColor().r;
    int gHexVal = sprite->getColor().g;
    int bHexVal = sprite->getColor().b;

    ImGui::Text("Texture");
    ImGui::ImageButton(("##EntityButton" + ID).c_str(), *sprite->getTexture(), { 48.f, 48.f }, sf::Color::Transparent, sf::Color::White);
    //ImGui::SameLine();
    //if (ImGui::Checkbox(("Repeat##SetRepeat" + ID).c_str(), &isRepeated))
    //{
    //	m_reg->emplace<Sprite>(entityID, m_data->textureManager["player"], isRepeated);
    //	sprite = m_reg->get<Sprite>(entityID).sprite;
    //}

    ImGui::NewLine();
    ImGui::Text("Color");
    ImGui::SameLine(divider1Pos);
    ImGui::Text("Origin");
    ImGui::SameLine(divider2Pos);
    ImGui::Text("Dimension");

    auto rHexValInput = ImGui::InputInt(("R##SpColorHexInput" + ID).c_str(), &rHexVal);
    ImGui::SameLine(divider1Pos);
    auto xOriginInput = ImGui::InputFloat(("X##TfOriginInput" + ID).c_str(), &xOrigin);
    ImGui::SameLine(divider2Pos);
    auto textureWidthInput = ImGui::InputInt(("Width##TfTextureInput" + ID).c_str(), &xTextureWidth);

    auto gHexValInput = ImGui::InputInt(("G##SpColorHexInput" + ID).c_str(), &gHexVal);
    ImGui::SameLine(divider1Pos);
    auto yOriginInput = ImGui::InputFloat(("Y##TfOriginInput" + ID).c_str(), &yOrigin);
    ImGui::SameLine(divider2Pos);
    auto textureHeightInput = ImGui::InputInt(("Height##TfTextureInput" + ID).c_str(), &yTextureHeight);

    auto bHexValInput = ImGui::InputInt(("B##SpColorHexInput" + ID).c_str(), &bHexVal);

    if (xOriginInput || yOriginInput)
        sprite->setOrigin(xOrigin, yOrigin);

    if (textureWidthInput || textureHeightInput)
        sprite->setTextureRect(sf::IntRect(0, 0, xTextureWidth, yTextureHeight));

    if (rHexValInput || gHexValInput || bHexValInput)
        sprite->setColor(sf::Color(rHexVal, gHexVal, bHexVal));

    ImGui::NewLine();
    ImGui::SeparatorText("Transform");

    float xPos = sprite->getPosition().x;
    float yPos = sprite->getPosition().y;
    float rotationDegree = sprite->getRotation();
    float xScale = sprite->getScale().x;
    float yScale = sprite->getScale().y;

    ImGui::Text("Position");
    ImGui::SameLine(divider1Pos);
    ImGui::Text("Rotation");
    ImGui::SameLine(divider2Pos);
    ImGui::Text("Scale");

    auto xPosInput = ImGui::InputFloat(("X##TfPosInput" + ID).c_str(), &xPos);
    ImGui::SameLine(divider1Pos);
    auto degRotationInput = ImGui::InputFloat(("Deg##TfRotationInput" + ID).c_str(), &rotationDegree);
    ImGui::SameLine(divider2Pos);
    auto xScaleInput = ImGui::InputFloat(("X##TfScaleInput" + ID).c_str(), &xScale);

    auto yPosInput = ImGui::InputFloat(("Y##TfPosInput" + ID).c_str(), &yPos);
    ImGui::SameLine(divider1Pos);
    auto degRotationSlider = ImGui::SliderFloat(("Deg##TfRotationSlider" + ID).c_str(), &rotationDegree, -360.f, 360.f);
    ImGui::SameLine(divider2Pos);
    auto yScaleInput = ImGui::InputFloat(("Y##TfScaleInput" + ID).c_str(), &yScale);

    if (xPosInput || yPosInput)
        sprite->setPosition(xPos, yPos);

    if (degRotationInput || degRotationSlider)
        sprite->setRotation(rotationDegree);

    if (xScaleInput || yScaleInput)
        sprite->setScale(xScale, yScale);

    ImGui::PopItemWidth();

    ImGui::NewLine();
}

void EditorComponentVisitor::visit(StatusModEvent* statusModEvent, entt::entity entityID)
{
    // No implementation
}

void EditorComponentVisitor::visit(TeamTag* teamTag, entt::entity entityID)
{
    ImGui::NewLine();

    const std::string ID = std::to_string(static_cast<unsigned int>(entityID));

    ImGui::Text("Team");
    const char* teamTypes[] = { "UNAFFILIATED", "FRIENDLY", "ENEMY" }; // NOTE: Update this when adding new team tags

    int selectedTeamType = static_cast<int>(teamTag->tag);
    if (ImGui::Combo(("##TeamType" + ID).c_str(), &selectedTeamType, teamTypes, IM_ARRAYSIZE(teamTypes)))
        teamTag->tag = static_cast<Team>(selectedTeamType);

    ImGui::NewLine();
}

void EditorComponentVisitor::visit(UpdateEntityEvent* updateEntityEvent, entt::entity entityID)
{
    ImGui::NewLine();

    // Visiting UpdateEntityEvent is unstable because of frequent add and removal from entities
    const std::string ID = std::to_string(static_cast<unsigned int>(entityID));

    ImGui::PushItemWidth((ImGui::GetWindowWidth() / 4.f) - 25.f);

    ImGui::Text("I-Frame duration");
    int maxDuration = static_cast<int>(updateEntityEvent->maxDuration.count());
    if (ImGui::InputInt(("Sec##EventDurInput" + ID).c_str(), &maxDuration))
        updateEntityEvent->maxDuration = std::chrono::milliseconds(maxDuration);

    ImGui::PopItemWidth();

    ImGui::NewLine();
}

void EditorComponentVisitor::visit(UpdateEntityPolling* updateEntityPolling, entt::entity entityID)
{
    ImGui::NewLine();

    const std::string ID = std::to_string(static_cast<unsigned int>(entityID));

    ImGui::PushItemWidth((ImGui::GetWindowWidth() / 4.f) - 25.f);

    ImGui::Text("I-Frame duration");
    int maxDuration = static_cast<int>(updateEntityPolling->maxDuration.count());
    if (ImGui::InputInt(("Sec##PollDurInput" + ID).c_str(), &maxDuration))
        updateEntityPolling->maxDuration = std::chrono::milliseconds(maxDuration);

    ImGui::PopItemWidth();

    ImGui::NewLine();
}