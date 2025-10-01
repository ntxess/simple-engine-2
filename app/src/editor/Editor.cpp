#include "Editor.hpp"

Editor::Editor()
    : m_appContext{nullptr}
    , m_panelFlags{0}
    , m_dockspaceId1{0}
    , m_dockspaceId2{0}
    , m_dockspaceId3{0}
    , m_dockspaceId4{0}
    , m_dockspaceId5{0}
    , m_enableEntityID{false}
    , m_enableEntityCollider{false}
    , m_enableEntityHeading{false}
    , m_enableEntityPosition{false}
    , m_enableQuadTreeVisualizer{false}
    , m_enableLogViewer{false}
    , m_startButtonEnabled{false}
    , m_forwardFrameEnabled{false}
    , m_selectedSceneData{nullptr}
{}

Editor::Editor(ApplicationContext* sysData)
    : Editor()
{
    m_appContext = sysData;
}

Editor::~Editor()
{
    // ImGui::SFML::Shutdown();
}

void Editor::init()
{
    auto fontConfigPath = m_appContext->configDataSerializer.load("config/font.json")->get<std::string>("default_font").value();
    auto path = std::filesystem::current_path() / fontConfigPath;
    if (!m_defaultFont.loadFromFile(path))
    {
        LOG_FATAL(Logger::get()) << "Failed to load default font.";
    }

    sf::ContextSettings settings;
    settings.depthBits = 24;
    settings.stencilBits = 8;
    settings.antiAliasingLevel = 0;
    settings.majorVersion = 4;
    settings.minorVersion = 3;

    // Disable flags for the dockspace panels; Make panel non-interactive 
    m_panelFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNavInputs |
        ImGuiWindowFlags_NoNavFocus;

    // Enable and setup dockspaces
    if (!ImGui::SFML::Init(m_appContext->window))
    {
        LOG_FATAL(Logger::get()) << "Failed to initialize ImGui-SFML.";
    }
    
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigDockingAlwaysTabBar = true; // ImGUI bug: Setting this true, will prevent you from using SetNextWindowSizeConstraints
                                         // which is needed for ratio-resizing of the scene view rendering panel
    
    // Initializing all the scenes for selection
    int width = m_appContext->configData.get<int>("width").value();
    int height = m_appContext->configData.get<int>("height").value();

    m_editorSceneMap["Sandbox"] = std::make_unique<EditorSceneAdapter>(
        std::make_unique<Sandbox>(m_appContext),
        width,
        height,
        settings
    );

    // m_editorSceneMap["MainMenu"] = std::make_unique<EditorSceneAdapter>(
    //     std::make_unique<MainMenu>(m_appContext),
    //     width,
    //     height,
    //     settings
    // );

    // m_editorSceneMap["GameOfLifeSim"] = std::make_unique<EditorSceneAdapter>(
    //     std::make_unique<GameOfLifeSim>(m_appContext),
    //     width,
    //     height,
    //     settings
    // );

    // Load in first scene of map
    m_selectedSceneKey = m_editorSceneMap.begin()->first;
    m_selectedSceneData = m_editorSceneMap.begin()->second.get();
    m_gameView = std::make_unique<sf::Sprite>(m_selectedSceneData->getRenderTexture().getTexture());

    // Register Component visitors for properties panel
    setupComponentVisitors(&m_componentVisitor);
}

void Editor::processEvent(const sf::Event& event)
{
    ImGui::SFML::ProcessEvent(m_appContext->window, event);
}

void Editor::processInput()
{
    if (m_startButtonEnabled)
        m_selectedSceneData->processInput();
}

void Editor::update()
{
    if (m_startButtonEnabled || m_forwardFrameEnabled) {
        m_selectedSceneData->update();
        m_forwardFrameEnabled = false;
    }
}

void Editor::render()
{
    ImGui::SFML::Update(m_appContext->window, sf::seconds(m_appContext->deltaTime));

    // Setup dockspace IDs
    m_dockspaceId1 = ImGui::GetID("Dockspace1");
    m_dockspaceId2 = ImGui::GetID("Dockspace2");
    m_dockspaceId3 = ImGui::GetID("Dockspace3");
    m_dockspaceId4 = ImGui::GetID("Dockspace4");
    m_dockspaceId5 = ImGui::GetID("Dockspace5");

    // Style modifications
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.f, 0.f });

    // Setup docked panels
    const float width = static_cast<float>(m_appContext->window.getSize().x);
    const float height = static_cast<float>(m_appContext->window.getSize().y);
    const float fifthWidth = width / 5.f;
    const float halfHeight = height / 2.f;
    const float scalingWidth = width / m_appContext->aspectRatio;
    const float scalingHeight = height / m_appContext->aspectRatio;
    const float scalingFifthWidth = fifthWidth + scalingWidth;

    setupDockPanel({ 0.f, 0.f }, { fifthWidth, halfHeight }, "##LP1", m_dockspaceId1);
    setupDockPanel({ 0.f, halfHeight }, { fifthWidth, halfHeight }, "##LP2", m_dockspaceId2);
    setupDockPanel({ fifthWidth, 0.f }, { scalingWidth, scalingHeight }, "##MP1", m_dockspaceId3);
    setupDockPanel({ fifthWidth, scalingHeight }, { scalingWidth, height - scalingHeight }, "##MP2", m_dockspaceId4);
    setupDockPanel({ scalingFifthWidth, 0.f }, { width - scalingFifthWidth, height }, "##RP1", m_dockspaceId5);

    ImGui::PopStyleVar(3);

    // Render each panel
    renderDebugPanel({ 0.f, 0.f }, { fifthWidth, halfHeight });
    renderPerformancePanel({ 0.f, 0.f }, { fifthWidth, halfHeight });
    renderLogViewPanel({ 0.f, halfHeight }, { fifthWidth, halfHeight });
    renderSceneViewPanel({ fifthWidth, 0.f }, { width, height });
    renderAssetsExplorerPanel({ fifthWidth, scalingHeight }, { scalingWidth, height - scalingHeight });
    renderPropertiesPanel({ scalingFifthWidth, 0.f }, { width - scalingFifthWidth, height });

    // ImGui::ShowDemoWindow();

    ImGui::SFML::Render(m_appContext->window);
}

void Editor::pause()
{
    // Nothing needed for pause
}

void Editor::resume()
{
    // Nothing needed for resume
}

void Editor::setApplicationContext(ApplicationContext* data)
{
    m_appContext = data;
}

void Editor::accept(ISceneVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}

entt::registry& Editor::getRegistry()
{
    if (m_selectedSceneData)
        return m_selectedSceneData->getRegistry();
    else
		throw std::runtime_error("No selected scene data available to get registry from.");
}

void Editor::setupComponentVisitors(IComponentVisitor* visitor)
{
    // Update this whenever we add a new renderable component
    registerComponentVisitor<Sprite>(visitor);
    registerComponentVisitor<UpdateEntityPolling>(visitor);
    registerComponentVisitor<UpdateEntityEvent>(visitor);
    registerComponentVisitor<EntityStatus>(visitor);
    registerComponentVisitor<EffectsList>(visitor);
    registerComponentVisitor<MovementPattern>(visitor, [this](const entt::entity& entity) {
        this->wayPointCanvasCallback(entity);
    });
    registerComponentVisitor<TeamTag>(visitor);
}

void Editor::setupDockPanel(const ImVec2& panPos, const ImVec2& panSize, const char* panID, const ImGuiID& dockID) const
{
    ImGui::SetNextWindowPos(panPos);
    ImGui::SetNextWindowSize(panSize);
    ImGui::Begin(panID, nullptr, m_panelFlags);
    ImGui::DockSpace(dockID, panSize, ImGuiDockNodeFlags_NoResize);
    ImGui::End();
}

void Editor::renderDebugPanel(const ImVec2& pos, const ImVec2& size)
{
    ImGui::SetNextWindowDockID(m_dockspaceId1, ImGuiCond_Once);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Once);
    ImGui::SetNextWindowSize(size, ImGuiCond_Once);
    ImGui::Begin("Debug Panel", nullptr, 0);

    const float totalWidth = ImGui::GetContentRegionAvail().x - 16.f;
    const float buttonWidth = totalWidth / 3.f;
    const float buttonHeight = 20.f;

    if (ImGui::Button("Play", { buttonWidth, buttonHeight }))
    {
        m_startButtonEnabled = true;
        m_forwardFrameEnabled = false;
    }

    ImGui::SameLine();

    if (ImGui::Button("Pause", { buttonWidth, buttonHeight }))
    {
        m_startButtonEnabled = false;
        m_forwardFrameEnabled = false;
    }

    ImGui::SameLine();

    if (ImGui::Button("Forward", { buttonWidth, buttonHeight }))
    {
        m_startButtonEnabled = false;
        m_forwardFrameEnabled = true;
    }

    if (ImGui::CollapsingHeader("Scene View Option"))
    {
        if (ImGui::BeginTable("split", 1))
        {
            ImGui::TableNextColumn();
            ImGui::Checkbox("Display Entity ID", &m_enableEntityID);
            ImGui::TableNextColumn();
            ImGui::Checkbox("Display Entity Box Visualizer", &m_enableEntityCollider);
            ImGui::TableNextColumn();
            ImGui::Checkbox("Display Entity Heading", &m_enableEntityHeading);
            ImGui::TableNextColumn();
            ImGui::Checkbox("Display Entity Position", &m_enableEntityPosition);
            ImGui::TableNextColumn();
            ImGui::Checkbox("Display Quad-Tree Visualizer", &m_enableQuadTreeVisualizer);

            ImGui::EndTable();
            
            ImGui::BeginDisabled(m_startButtonEnabled);
            static int selectedEntityAmount = 60;
            static int prevEntityAmount = 0;

            ImGui::SliderInt("Entity Amount", &selectedEntityAmount, 0, 1000);

            if (ImGui::Button("Replace Entity", { buttonWidth, buttonHeight }))
            {
                auto view = m_selectedSceneData->getRegistry().view<TeamTag>();
                for (const auto& entityID : view)
                {
                    auto& team = m_selectedSceneData->getRegistry().get<TeamTag>(entityID);
                    if (team.tag == Team::ENEMY)
                    {
                        m_selectedSceneData->getRegistry().destroy(entityID);
                        m_selectedSceneData->get()->accept(&m_sceneModifierVisitor, entityID);
                    }
                }

                // Generate a ton of sprite for testing in random places within the boundary of the window
                generateEntities(selectedEntityAmount);
                prevEntityAmount = selectedEntityAmount;
            }

            ImGui::SameLine();

            if (ImGui::Button("Generate Entity", { buttonWidth, buttonHeight }))
            {
                generateEntities(selectedEntityAmount);
                prevEntityAmount += selectedEntityAmount;
            }

            ImGui::SameLine();

            if (ImGui::Button("Remove Entity", { buttonWidth, buttonHeight }))
            {
                // Destroy all of the old generated entities
                auto view = m_selectedSceneData->getRegistry().view<TeamTag>();
                for (const auto& entityID : view)
                {
                    auto& team = m_selectedSceneData->getRegistry().get<TeamTag>(entityID);
                    if (team.tag == Team::ENEMY)
                    {
                        m_selectedSceneData->getRegistry().destroy(entityID);
                        m_selectedSceneData->get()->accept(&m_sceneModifierVisitor, entityID);
                    }
                }
                prevEntityAmount = 0;
            }

            static int frameRateTarget = 60;
            if (ImGui::SliderInt("Frame Rate", &frameRateTarget, 0, 255))
                m_appContext->deltaTime = static_cast<float>(1000.f / frameRateTarget);
            ImGui::EndDisabled();
        }
    }

    if (ImGui::CollapsingHeader("Scene Manager"))
    {
        for (const auto& [sceneName, sceneObj] : m_editorSceneMap)
        {
            if (ImGui::Selectable(sceneName.c_str(), m_selectedSceneKey == sceneName))
            {
                m_selectedSceneKey = sceneName;
                m_selectedSceneData = m_editorSceneMap.at(sceneName).get();
            }
        }
    }

    if (ImGui::CollapsingHeader("Animation Manager"))
    {

    }

    if (ImGui::CollapsingHeader("Resource Manager"))
    {

    }

    if (ImGui::CollapsingHeader("Save Manager"))
    {

    }

    ImGui::End();
}

void Editor::renderPerformancePanel(const ImVec2& pos, const ImVec2& size)
{
    ImGui::SetNextWindowDockID(m_dockspaceId1, ImGuiCond_Once);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Once);
    ImGui::SetNextWindowSize(size, ImGuiCond_Once);
    ImGui::Begin("Performance Panel", nullptr, 0);

    m_selectedSceneData->get()->accept(&m_sceneSystemVisitor, entt::null);

    ImGui::End();
}

void Editor::renderLogViewPanel(const ImVec2& pos, const ImVec2& size)
{
    ImGui::SetNextWindowDockID(m_dockspaceId2, ImGuiCond_Once);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Once);
    ImGui::SetNextWindowSize(size, ImGuiCond_Once);
    ImGui::Begin("Log View Panel", nullptr);

    static bool autoScroll = true;

    if (ImGui::Button(m_enableLogViewer ? "Disable Log Viewer" : "Enable Log Viewer"))
    {
        m_enableLogViewer = !m_enableLogViewer;
        m_logStream.open(Logger::getFileName());
    }
    ImGui::SameLine();
    ImGui::Checkbox("Enable Auto-Scrolling", &autoScroll);

    if (m_enableLogViewer)
    {
        ImGui::BeginChild("##Log", { 0, 0 }, 0, ImGuiWindowFlags_HorizontalScrollbar);

        m_logStream.update();

        for (const auto& log : m_logStream.getLog())
        {
            ImVec4 color;
            switch (log.type)
            {
            case LogStream::LogType::None:    color = { 1.0f, 1.0f, 1.0f, 1.0f }; break; // White
            case LogStream::LogType::Trace:   color = { 1.0f, 1.0f, 1.0f, 1.0f }; break; // White
            case LogStream::LogType::Debug:   color = { 0.5f, 0.5f, 1.0f, 1.0f }; break; // Light blue
            case LogStream::LogType::Info:    color = { 0.5f, 0.5f, 1.0f, 1.0f }; break; // Light blue
            case LogStream::LogType::Warning: color = { 1.0f, 1.0f, 0.0f, 1.0f }; break; // Yellow
            case LogStream::LogType::Error:   color = { 1.0f, 0.0f, 0.0f, 1.0f }; break; // Red
            case LogStream::LogType::Fatal:   color = { 1.0f, 0.0f, 0.0f, 1.0f }; break; // Red
            }

            ImGui::TextColored(color, "%s", log.text.c_str());
        }

        if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);

        ImGui::EndChild();
    }

    ImGui::End();
}

void Editor::renderSceneViewPanel(const ImVec2& pos, const ImVec2& size)
{
    ImGui::SetNextWindowDockID(m_dockspaceId3, ImGuiCond_Once);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Once);
    ImGui::SetNextWindowSize(size, ImGuiCond_Once);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.f, 0.f });
    ImGui::Begin("Scene View Panel", nullptr, 0);
    ImGui::PopStyleVar();

    auto& renderTexture = m_selectedSceneData->getRenderTexture();

    // Clear the previous buffer then call to the actual scenes render function
    renderTexture.clear();
    m_selectedSceneData->render();

    displayEntityVisualizers();
    displayCollisionSystemVisualizer();

    // Get free space inside the window and calculate scene size and scale with aspect ratio
    ImVec2 region = ImGui::GetContentRegionAvail();
    ImVec2 drawSize = scaleSize(region, m_appContext->aspectRatio);
    ImVec2 viewPos = getCenteredTLPos(region, m_appContext->aspectRatio);

    // Draw the scene texture with correct scaling
    m_gameView->setTexture(renderTexture.getTexture(), true);
    m_gameView->setScale({
        drawSize.x / m_gameView->getTexture().getSize().x,
        drawSize.y / m_gameView->getTexture().getSize().y
    });

    // Render the selected scene as an image inside ImGui window
    ImGui::SetCursorScreenPos(viewPos);
    ImGui::Image(*m_gameView);

    // Update the memeber variables for the WayPoint Canvas to stay consistant with scene view size and scale
    m_sceneDrawScale.x = drawSize.x / m_gameView->getTexture().getSize().x;
    m_sceneDrawScale.y = drawSize.y / m_gameView->getTexture().getSize().y;

    ImGui::End();
}

void Editor::renderAssetsExplorerPanel(const ImVec2& pos, const ImVec2& size)
{
    ImGui::SetNextWindowDockID(m_dockspaceId4, ImGuiCond_Once);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Once);
    ImGui::SetNextWindowSize(size, ImGuiCond_Once);
    ImGui::Begin("AssetsExplorer Panel", nullptr, 0);

    ImGui::End();
}

void Editor::renderPropertiesPanel(const ImVec2& pos, const ImVec2& size)
{
    ImGui::SetNextWindowDockID(m_dockspaceId5, ImGuiCond_Once);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Once);
    ImGui::SetNextWindowSize(size, ImGuiCond_Once);
    ImGui::Begin("Properties Panel", nullptr, ImGuiWindowFlags_AlwaysVerticalScrollbar);

    std::vector<entt::entity> entitiesToErase;
    const auto& view = m_selectedSceneData->getRegistry().view<entt::entity>();
    for (const auto& entityID : view)
    {
        if (m_selectedSceneData->entities.find(entityID) == m_selectedSceneData->entities.end()) continue;

        auto& [isEntityPanelOpen, entityProp] = m_selectedSceneData->entities.at(entityID);

        if (ImGui::CollapsingHeader(entityProp.name.c_str(), &isEntityPanelOpen))
        {
            ImGui::BeginDisabled(m_startButtonEnabled);
            ImGui::BeginChild(("##EntityColm" + entityProp.name).c_str(), { ImGui::GetWindowWidth() - 26.f, 0.f }, ImGuiChildFlags_AutoResizeY);

            for (size_t i = 0; i < entityProp.components.size(); ++i)
            {
                if (ImGui::CollapsingHeader((entityProp.components[i].second.name() + std::string("##") + entityProp.name).c_str(), &entityProp.components[i].first, ImGuiTreeNodeFlags_DefaultOpen))
                {
                    if (m_selectedSceneData->renderFunc.find(entityProp.components[i].second) == m_selectedSceneData->renderFunc.end()) continue;

                    m_selectedSceneData->renderFunc.at(entityProp.components[i].second)(m_selectedSceneData->getRegistry(), entityID);
                }

                if (!entityProp.components[i].first)
                {
                    std::type_index typeIndex = entityProp.components[i].second;
                    if (typeIndex == typeid(Sprite))
                    {
                        m_selectedSceneData->get()->accept(&m_sceneModifierVisitor, entityID);
                    }
                    entityProp.components.erase(entityProp.components.begin() + i);
                    LOG_DEBUG(Logger::get()) << "Removed component [" << typeIndex.name() << "] from entity [" << static_cast<unsigned int>(entityID) << "]";
                    --i;
				}
            }

            ImGui::EndChild();
            ImGui::EndDisabled();
        }

		if (!isEntityPanelOpen)
            entitiesToErase.push_back(entityID);
    }

    for (const auto& entityID : entitiesToErase)
    {
	    m_selectedSceneData->getRegistry().destroy(entityID);
	    m_selectedSceneData->entities.erase(entityID);
    }

    ImGui::End();
}

void Editor::displayEntityVisualizers()
{
    auto& sceneRenderTexture = m_selectedSceneData->getRenderTexture();
    const auto& view = m_selectedSceneData->getRegistry().view<Sprite>();
    for (const auto& entityID : view)
    {
        auto& spriteEntity = view.get<Sprite>(entityID);

        if (m_enableEntityID)
        {
            sf::String ID(std::to_string(static_cast<int>(entityID)));
            sf::Text IDText(m_defaultFont, ID, 20);
            IDText.setPosition({
                spriteEntity.getPosition().x + spriteEntity.getGlobalBounds().size.x,
                spriteEntity.getPosition().y + spriteEntity.getGlobalBounds().size.y
            });
            sceneRenderTexture.draw(IDText);
        }

        if (m_enableEntityCollider)
        {
            const auto& globalBounds = spriteEntity.getGlobalBounds();
            sf::RectangleShape border({ globalBounds.position.x, globalBounds.position.y });
            border.setOrigin({ globalBounds.position.x / 2.f, globalBounds.position.y / 2.f });
            border.setPosition(spriteEntity.getPosition());
            border.setRotation(spriteEntity.getRotation());
            border.setFillColor(sf::Color::Transparent);
            border.setOutlineThickness(2);

            // TODO: Remove findEntityID for a more robust solution
            static entt::entity player = findEntityID<PlayerInput>();
            if (player != entt::null &&
                player != entityID &&
                m_selectedSceneData->getRegistry().get<Sprite>(player).getGlobalBounds().findIntersection(spriteEntity.getGlobalBounds()))
            {
                border.setOutlineColor(sf::Color::Red);
                LOG_TRACE(Logger::get())
                    << "Render block found collision between ["
                    << static_cast<unsigned int>(player)
                    << "] and ["
                    << static_cast<unsigned int>(entityID) << "]";
            }
            else
            {
                border.setOutlineColor(sf::Color::Green);
            }
            sceneRenderTexture.draw(border);
        }

        if (m_enableEntityHeading)
        {
            sf::RectangleShape northHeading({ 50, 2 });
            sf::RectangleShape eastHeading({ 50, 2 });
            northHeading.setPosition(spriteEntity.getPosition());
            northHeading.setRotation(spriteEntity.getRotation() - sf::degrees(90));
            eastHeading.setPosition(spriteEntity.getPosition());
            eastHeading.setRotation(spriteEntity.getRotation());
            sceneRenderTexture.draw(northHeading);
            sceneRenderTexture.draw(eastHeading);
        }

        if (m_enableEntityPosition)
        {
            std::ostringstream oss;
            oss << "\t "
                << std::fixed << std::setprecision(2)
                << spriteEntity.getPosition().x << ", "
                << spriteEntity.getPosition().y;

            sf::String ID(oss.str());
            sf::Text IDText(m_defaultFont, ID, 20);
            IDText.setPosition({
                spriteEntity.getPosition().x + spriteEntity.getGlobalBounds().size.x,
                spriteEntity.getPosition().y
            });
            sceneRenderTexture.draw(IDText);
        }
    }
}

void Editor::displayCollisionSystemVisualizer()
{
    if (m_enableQuadTreeVisualizer)
    {
        auto& sceneRenderTexture = m_selectedSceneData->getRenderTexture();
        auto manager = static_cast<Sandbox*>(m_selectedSceneData->get())->getSystemManager();
        manager->getSystem<CollisionSystem>()->draw(sceneRenderTexture);
    }
}

ImVec2 Editor::scaleSize(const ImVec2& region, const float& aspectRatio)
{
    float drawWidth = region.x;
    float drawHeight = drawWidth / aspectRatio;
    if (drawHeight > region.y)
    {
        drawHeight = region.y;
        drawWidth = drawHeight * aspectRatio;
    }
    return { drawWidth, drawHeight };
}

ImVec2 Editor::getCenteredTLPos(const ImVec2& region, const float& aspectRatio)
{
    ImVec2 drawSize = scaleSize(region, aspectRatio);

    // Center the image in the window content area
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    ImVec2 viewPos(
        cursorPos.x + (region.x - drawSize.x) / 2.f,
        cursorPos.y + (region.y - drawSize.y) / 2.f
    );

    return viewPos;
}

void Editor::wayPointCanvasCallback(const entt::entity& entityID)
{
    auto& [_, compProp] = m_selectedSceneData->entities.at(entityID);

    if (ImGui::Button(
        ("Set New Path##PathDesigner" + compProp.name).c_str(),
        { ImGui::GetWindowWidth(), 22.f }
    ))
    {
        compProp.isWaypointEditorOpen = true;
    }
    ImGui::NewLine();

    if (compProp.isWaypointEditorOpen)
    {
        drawWayPointCanvas(entityID, compProp);
    }
}

void Editor::drawWayPointCanvas(const entt::entity& entityID, ComponentPropData& cmpntData)
{
    ImGui::SetNextWindowDockID(m_dockspaceId3, ImGuiCond_Once);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.f, 0.f });
    ImGui::Begin(("WayPoint Editor | " + cmpntData.name).c_str(), &cmpntData.isWaypointEditorOpen, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    // Center the image in the window content area
    ImVec2 gameViewSize = { static_cast<float>(m_gameView->getTexture().getSize().x), static_cast<float>(m_gameView->getTexture().getSize().y) };
    ImVec2 canvasSize = { gameViewSize.x * m_sceneDrawScale.x, gameViewSize.y * m_sceneDrawScale.y };
    ImVec2 canvasPosTL = getCenteredTLPos(ImGui::GetContentRegionAvail(), m_appContext->aspectRatio);
    ImVec2 canvasPosBR = { canvasPosTL.x + canvasSize.x, canvasPosTL.y + canvasSize.y };
    ImVec2 origin = { canvasPosTL.x + cmpntData.scrolling.x, canvasPosTL.y + cmpntData.scrolling.y };

    // Process the inputs
    processWayPointCanvasInput(canvasSize, origin, entityID, cmpntData);

    // Draw the render scene as the background, pan and zoom alongside canvas
    ImGui::SetCursorScreenPos({ canvasPosTL.x + cmpntData.scrolling.x, canvasPosTL.y + cmpntData.scrolling.y });
    ImGui::Image(*m_gameView, { canvasSize.x * cmpntData.zoom, canvasSize.y * cmpntData.zoom });

    // Draw the canvas
    drawWayPointContextMenu(entityID, cmpntData);
    drawWayPoints(canvasPosTL, canvasPosBR, canvasSize, origin, cmpntData);

    ImGui::SetCursorScreenPos(canvasPosTL);
    ImGui::Text(" Mouse Left: click to add waypoints, Mouse Right: drag to pan, click for context menu.");
    ImGui::SameLine();

    ImGui::End();
}

void Editor::drawWayPoints(const ImVec2& tlBound, const ImVec2& brBound, const ImVec2& size, const ImVec2& origin, ComponentPropData& cmpntData)
{
    // Draw grid lines
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(tlBound, brBound, true);

    const float gridStep = 32.0f;
    for (float x = fmodf(cmpntData.scrolling.x * cmpntData.zoom, gridStep * cmpntData.zoom); x < size.x; x += gridStep * cmpntData.zoom)
    {
        drawList->AddLine(
            { tlBound.x + x, tlBound.y },
            { tlBound.x + x, brBound.y },
            IM_COL32(200, 200, 200, 40));
    }

    for (float y = fmodf(cmpntData.scrolling.y * cmpntData.zoom, gridStep * cmpntData.zoom); y < size.y; y += gridStep * cmpntData.zoom)
    {
        drawList->AddLine(
            { tlBound.x, tlBound.y + y },
            { brBound.x, tlBound.y + y },
            IM_COL32(200, 200, 200, 40));
    }

    // Draw lines from points
    for (int i = 0; i < cmpntData.points.Size; i++)
    {
        if (i < cmpntData.points.Size - 1)
        {
            drawList->AddLine(
                {
                    origin.x + cmpntData.points[i].x * m_sceneDrawScale.x * cmpntData.zoom,
                    origin.y + cmpntData.points[i].y * m_sceneDrawScale.y * cmpntData.zoom
                },
                {
                    origin.x + cmpntData.points[i + 1].x * m_sceneDrawScale.x * cmpntData.zoom,
                    origin.y + cmpntData.points[i + 1].y * m_sceneDrawScale.y * cmpntData.zoom
                },
                IM_COL32(255, 255, 0, 255), 2.0f
            );
        }

        // Set the draw cursor for rendering the label next to the point 
        ImVec2 labelPos = {
            origin.x + cmpntData.points[i].x * m_sceneDrawScale.x * cmpntData.zoom,
            origin.y + cmpntData.points[i].y * m_sceneDrawScale.y * cmpntData.zoom
        };

        ImGui::SetCursorScreenPos(labelPos);

        // Draw the coordinate label
        ImVec4 color = { 1.f, 1.f, 1.f, 1.f }; // Default white
        if (i == 0)                                // First waypoint will always be green
            color = { 0.f, 1.f, 0.f, 1.f };
        else if (i == cmpntData.points.Size - 1)   // Last waypoint will alaywas be red
            color = { 1.f, 0.f, 0.f, 1.f };

        ImGui::TextColored(color, "(%.2f, %.2f)", cmpntData.points[i].x, cmpntData.points[i].y);
    }

    drawList->PopClipRect();
}

void Editor::drawWayPointContextMenu(const entt::entity& entityID, ComponentPropData& cmpntData)
{
    if (ImGui::BeginPopup("context"))
    {
        if (ImGui::MenuItem("Remove one", nullptr, false, cmpntData.points.Size > 0))
        {
            cmpntData.points.resize(cmpntData.points.size() - 1);
            updateWayPointComponent(entityID, cmpntData, WayPointEditMode::RemoveLast);
        }

        if (ImGui::MenuItem("Remove all", nullptr, false, cmpntData.points.Size > 0))
        {
            cmpntData.points.clear();
            updateWayPointComponent(entityID, cmpntData, WayPointEditMode::Clear);
        }

        if (ImGui::MenuItem("Reset Camera", nullptr, false))
        {
            cmpntData.scrolling = { 0.f, 0.f };
            cmpntData.zoom = 1.0f;
        }

        ImGui::EndPopup();
    }
}

void Editor::updateWayPointCanvas(const entt::entity& entityID, ComponentPropData& cmpntData)
{
	// Populate the waypoint canvas with the current movement pattern of the entity
    if (m_selectedSceneData->getRegistry().all_of<MovementPattern, Sprite>(entityID))
    {
        auto& movement = m_selectedSceneData->getRegistry().get<MovementPattern>(entityID);

        WayPoint* current = movement.movePattern.get();

        if (!current) return;

        cmpntData.points.clear();
        cmpntData.points.push_back({ current->coordinate.x, current->coordinate.y });

        while (current->nextWP != nullptr)
        {
            current = current->next();
            cmpntData.points.push_back({ current->coordinate.x, current->coordinate.y });
        }
    }
}

void Editor::updateWayPointComponent(const entt::entity& entityID, ComponentPropData& cmpntData, WayPointEditMode mode)
{
    if (m_selectedSceneData->getRegistry().all_of<MovementPattern, Sprite>(entityID))
    {
        auto& sprite = m_selectedSceneData->getRegistry().get<Sprite>(entityID);
        auto& movement = m_selectedSceneData->getRegistry().get<MovementPattern>(entityID);

        // If the waypoint canvas editor is empty, set the starting waypoint to the current position of the sprite
        // This syncs the waypoint component with the waypoint canvas
        if (cmpntData.points.empty())
        {
            cmpntData.points.push_back({ sprite.getPosition().x, sprite.getPosition().y });
        }

        if (mode == WayPointEditMode::Clear)
        {
            // Sprite with a waypoint component but no next waypoint will still have the initial position of the sprite as its sole waypoint
            std::unique_ptr<WayPoint> root = std::make_unique<WayPoint>(sf::Vector2f{ sprite.getPosition().x, sprite.getPosition().y });
            movement.currentPath = root.get();
            movement.distance = 0.f;
            m_selectedSceneData->getRegistry().emplace_or_replace<MovementPattern>(entityID, std::move(root), true);
        }
        else
        {
            // Always reset the current movement progress to keep sprite position consistant 
            WayPoint* current = movement.movePattern.get();
            movement.currentPath = movement.movePattern.get();
            movement.distance = 0.f;
            sprite.setPosition({ current->coordinate.x, current->coordinate.y });

            // Find last and second to last waypoint
            WayPoint* prev = nullptr;
            while (current && current->next())
            {
                prev = current;
                current = current->next();
            }

            if (mode == WayPointEditMode::Add)
            {
                // Setup and link the new waypoint
                const auto& coord = cmpntData.points.back();
                std::unique_ptr<WayPoint> newPoint = std::make_unique<WayPoint>(sf::Vector2f{ coord.x, coord.y });

                // Sanity check but not required as there should always be atleast one waypoint in the movement pattern (the current sprite pos)
                if (current)
                {
                    current->link(std::move(newPoint));
                    current->nextWP->distanceTotal = current->distanceTotal + current->distanceToNext;
                }
            }
            else if (mode == WayPointEditMode::RemoveLast)
            {
                if (prev)
                {
                    // Delete the last node
                    prev->nextWP.reset();
                }
                else
                {
                    // Reset movement pattern to the current pos of sprite if last waypoint is removed
                    movement.movePattern = std::make_unique<WayPoint>(sf::Vector2f{ sprite.getPosition().x, sprite.getPosition().y });
                    movement.currentPath = movement.movePattern.get();
                }
            }
        }
    }
}

void Editor::processWayPointCanvasInput(const ImVec2& size, const ImVec2& origin, const entt::entity& entityID, ComponentPropData& cmpntData)
{
    // Canvas is a large button that will used to catch mouse interactions
    ImGui::InvisibleButton("canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool isHovered = ImGui::IsItemHovered();
    const bool isActive = ImGui::IsItemActive();

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mousePosInCanvas(
        (io.MousePos.x - origin.x) / (cmpntData.zoom * m_sceneDrawScale.x),
        (io.MousePos.y - origin.y) / (cmpntData.zoom * m_sceneDrawScale.y)
    );

    // Adjust camera scrolling using change in mouse position in canvas
    const float mouseThresholdForPan = -1.0f;
    if (isActive && ImGui::IsMouseDragging(ImGuiMouseButton_Right, mouseThresholdForPan))
    {
        cmpntData.scrolling.x += io.MouseDelta.x;
        cmpntData.scrolling.y += io.MouseDelta.y;
    }

    // Context menu (under default mouse threshold)
    ImVec2 dragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);

    if (dragDelta.x == 0.0f && dragDelta.y == 0.0f)
        ImGui::OpenPopupOnItemClick("context", ImGuiPopupFlags_MouseButtonRight);

    if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        cmpntData.points.push_back(mousePosInCanvas);
        updateWayPointComponent(entityID, cmpntData, WayPointEditMode::Add);
    }

    if (isHovered && io.MouseWheel != 0)
    {
        const float zoomSpeed = 0.1f;
        cmpntData.zoom += io.MouseWheel * zoomSpeed;

        // Clamp zoom to prevent flipping or disappearing
        cmpntData.zoom = ImClamp(cmpntData.zoom, 0.1f, 10.0f);
    }
}

void Editor::generateEntities(size_t numOfEntities)
{
    float width = static_cast<float>(m_appContext->configData.get<int>("width").value());
    float height = static_cast<float>(m_appContext->configData.get<int>("height").value());

    // Generate a ton of sprite for testing in random places within the boundary of the window
    std::random_device dev;
    std::mt19937 rng(dev());
    std::uniform_int_distribution<std::mt19937::result_type> dist6(0, static_cast<unsigned int>(width));

    for (size_t i = 0; i < numOfEntities; i++)
    {
        std::unique_ptr<WayPoint> root = nullptr;
        WayPoint* last = nullptr;

        for (size_t j = 0; j <= size_t(dist6(rng)) % 10; j++)
        {
            std::unique_ptr<WayPoint> point = std::make_unique<WayPoint>(sf::Vector2f{ float(dist6(rng) % int(width)), float(dist6(rng) % int(height)) });

            if (j != 0)
            {
                last->link(std::move(point));
                last->nextWP->distanceTotal = last->distanceTotal + last->distanceToNext;
                last = last->next();
            }
            else
            {
                root = std::move(point);
                last = root.get();
            }
        }

        // Entity create and store into the scene's ENTT::entity registry
        entt::entity mob = m_selectedSceneData->createEntity();
        const auto& texture = m_appContext->textureManager.get("player");
        if (!texture)
        {
            LOG_ERROR(Logger::get()) << "Failed to get texture for generated entity!";
            return;
        }
        m_selectedSceneData->getRegistry().emplace<Sprite>(mob, texture.value());
        m_selectedSceneData->getRegistry().get<Sprite>(mob).setPosition({ root->coordinate.x, root->coordinate.y });
        m_selectedSceneData->getRegistry().emplace<EntityStatus>(mob);
        m_selectedSceneData->getRegistry().get<EntityStatus>(mob).values["HP"] = 100.f;
        m_selectedSceneData->getRegistry().get<EntityStatus>(mob).values["Speed"] = 0.5f;
        m_selectedSceneData->getRegistry().emplace<TeamTag>(mob, Team::ENEMY);
        m_selectedSceneData->getRegistry().emplace<MovementPattern>(mob, std::move(root), true);
        m_selectedSceneData->getRegistry().emplace<UpdateEntityEvent>(mob);

        updateWayPointCanvas(mob, m_selectedSceneData->entities.at(mob).second);
    }
}
