#include "stage_panel.hpp"

#include "stage.hpp"

#include <algorithm>
#include <string>

namespace ajfx::studio {
namespace {

void note(StagePanelState& state, StageControl control) {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    state.where[static_cast<std::size_t>(control)] = ImVec4{min.x, min.y, max.x, max.y};
}

// A button that stays lit while its tool is the one picked.
bool tool_button(StagePanelState& state, StageControl control, const char* label, StageTool mine, StageTool& tool,
                 const char* tip) {
    const bool on = tool == mine;
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    const bool pressed = ImGui::Button(label);
    note(state, control);
    if (on) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tip);
    }
    if (pressed) {
        tool = on ? StageTool::Select : mine;
    }
    return pressed;
}

} // namespace

StageRequest draw_stage_panel(StagePanelState& state, const StagePanelIn& in, StageTool& tool, ImVec2 where) {
    StageRequest request = StageRequest::None;
    ImGui::SetNextWindowPos(where, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2{ImGui::GetFontSize() * 17.0F, 0.0F}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Stage", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return request;
    }
    const Scene* scene = in.document != nullptr && in.document->scene ? &*in.document->scene : nullptr;

    ImGui::TextDisabled("Click in the viewport to");
    tool_button(state, StageControl::Select, "Select", StageTool::Select, tool, "Click a thing to select it (Esc)");
    ImGui::SameLine();
    tool_button(state, StageControl::Place, "Place", StageTool::Place, tool,
                "Put a copy of the selected object on the stage, where it stands when the scene starts (O)");
    ImGui::SameLine();
    tool_button(state, StageControl::Drop, "Drop", StageTool::Drop, tool, "Drop a copy of the selected object in, at this moment of the scene (D)");
    ImGui::SameLine();
    tool_button(state, StageControl::Delete, "Delete", StageTool::Delete, tool, "Take away the thing you click from then on (X)");

    if (scene != nullptr) {
        const std::size_t placed = stage_count(scene->performance);
        const std::size_t held = dropped_count(scene->performance);
        const std::uint32_t most = scene->physics.max_dropped;
        ImGui::Spacing();
        ImGui::Text("%zu on the stage, %zu dropped", placed, held >= placed ? held - placed : 0U);
        ImGui::TextDisabled("room for %u of %u", most > held ? most - static_cast<std::uint32_t>(held) : 0U, most);

        ImGui::SeparatorText("Setup");
        const bool any = placed != 0U;
        const bool touched = std::any_of(scene->performance.acts.begin(), scene->performance.acts.end(),
                                         [](const Act& act) { return on_stage(act) && act.touched; });
        ImGui::BeginDisabled(!any || in.settling);
        if (ImGui::Button(in.settling ? "Settling..." : "Settle")) {
            request = StageRequest::Settle;
        }
        note(state, StageControl::Settle);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Let everything on the stage fall and come to rest, and keep it where it lands");
        }
        ImGui::SameLine();
        if (ImGui::Button("Jitter")) {
            request = StageRequest::Jitter;
        }
        note(state, StageControl::Jitter);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Nudge and tilt everything on the stage a little, so a neat stack loosens");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!touched || in.settling);
        if (ImGui::Button("Reset")) {
            request = StageRequest::Reset;
        }
        note(state, StageControl::Reset);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Put everything settled or jittered back where you put it");
        }
        ImGui::EndDisabled();

        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 4.5F);
        ImGui::SliderFloat("Shift", &state.shift, 0.0F, 0.5F, "%.2f m");
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 4.5F);
        ImGui::SliderFloat("Tilt", &state.tilt, 0.0F, 30.0F, "%.0f deg");
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 4.5F);
        ImGui::InputInt("Seed", &state.seed);
    }
    if (!in.status.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(in.status.data(), in.status.data() + in.status.size());
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
    return request;
}

} // namespace ajfx::studio
