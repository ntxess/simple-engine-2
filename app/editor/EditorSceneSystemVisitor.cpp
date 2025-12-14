#include "EditorSceneSystemVisitor.hpp"

void EditorSceneSystemVisitor::visit(Sandbox* sandbox, entt::entity entityID)
{
    auto timingHistoryMap = sandbox->getSystemManager()->getSystemTimingHistory();
    for (const auto& [systemID, historyDeque] : timingHistoryMap)
    {
        if (ImGui::TreeNode((systemID + " Profiler").c_str()))
        {
            std::vector<float> values(10, 0.0f);
            float avg = 0.0f;
            float peak = 0.0f;
            float current = 0.0f;

            if (!historyDeque.empty())
            {
                values.resize(historyDeque.size());
                float sum = 0.0f;
                for (size_t i = 0; i < historyDeque.size(); ++i)
                {
                    values[i] = static_cast<float>(historyDeque[i]) * 1e-6f;
                    sum += values[i];
                    if (values[i] > peak) peak = values[i];
                }
                avg = sum / values.size();
                current = values.back();
            }

            float scale_max = std::max({ avg * 1.5f, peak * 1.1f, 1.0f });

            ImVec2 graphSize(ImGui::GetContentRegionAvail().x - 40.0f, 120.0f);
            ImVec2 graphPos = ImGui::GetCursorScreenPos();

            // Main plot
            ImGui::PlotLines(
                ("##" + systemID).c_str(),
                values.data(),
                static_cast<int>(values.size()),
                0,
                nullptr,
                0.0f,
                scale_max,
                graphSize
            );

            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            const float plotWidth = graphSize.x;
            const float plotHeight = graphSize.y;

            auto mapValueY = [plotHeight, scale_max, graphPos](float v) {
                return graphPos.y + plotHeight * (1.0f - v / scale_max);
            };

            auto formatValue = [](float v) {
                std::stringstream ss;
                ss << std::fixed << std::setprecision(3) << v;
                return ss.str();
            };

            // Max/min labels on right side of the plot
            draw_list->AddText({ graphPos.x + plotWidth + 2, graphPos.y },
                IM_COL32(255, 255, 255, 255),
                formatValue(scale_max).c_str() // max
            ); 

            draw_list->AddText({ graphPos.x + plotWidth + 2, graphPos.y + plotHeight - 12 },
                IM_COL32(255, 255, 255, 255),
                "0.000" // min
            );

            // Hover overlay for horizontal lines
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Text("Avg:  %.3f ms", avg);
                ImGui::Text("Peak: %.3f ms", peak);
                ImGui::Text("Curr: %.3f ms", current);
                ImGui::EndTooltip();

                draw_list->AddLine({ graphPos.x, mapValueY(avg) },
                    ImVec2(graphPos.x + plotWidth, mapValueY(avg)),
                    IM_COL32(255, 255, 0, 255), 1.0f);
                draw_list->AddLine({ graphPos.x, mapValueY(peak) },
                    ImVec2(graphPos.x + plotWidth, mapValueY(peak)),
                    IM_COL32(255, 0, 0, 255), 1.0f);
                draw_list->AddLine({ graphPos.x, mapValueY(current) },
                    ImVec2(graphPos.x + plotWidth, mapValueY(current)),
                    IM_COL32(0, 255, 0, 255), 1.0f);
            }

            // Reserve vertical space for label box
            ImGui::Dummy({ 0, 55.0f }); // Enough size for stacked labels

            // Draw stacked labels inside the box
            float labelX = graphPos.x + 5.0f;
            float labelY = graphPos.y + plotHeight + 5.0f;
            float labelSpacing = 15.0f;

            draw_list->AddText({ labelX, labelY },
                IM_COL32(255, 255, 0, 255),
                ("Avg:  " + std::to_string(avg) + " ms").c_str());
            labelY += labelSpacing;
            draw_list->AddText({ labelX, labelY },
                IM_COL32(255, 0, 0, 255),
                ("Peak: " + std::to_string(peak) + " ms").c_str());
            labelY += labelSpacing;
            draw_list->AddText({ labelX, labelY },
                IM_COL32(0, 255, 0, 255),
                ("Curr: " + std::to_string(current) + " ms").c_str());

            ImGui::TreePop();
        }
    }
}

void EditorSceneSystemVisitor::visit(MainMenu* mainMenu, entt::entity entityID)
{
}

void EditorSceneSystemVisitor::visit(GameOfLifeSim* gameOfLifeSim, entt::entity entityID)
{
}

void EditorSceneSystemVisitor::visit(Editor* editor, entt::entity entityID)
{
}