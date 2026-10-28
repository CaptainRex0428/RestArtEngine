#include <RestArtEngine/Editor/SceneEditor.h>
#include <imgui.h>

#include <algorithm>
#include <cfloat>

namespace rae::editor {

void SceneEditor::syncProjectSettings(const renderer::SceneSettings& settings) {
    projectRenderingDraft_.hardwareRayTracing = settings.hardwareRayTracing;
    projectRenderingDraft_.hardwareRayTracingReflections = settings.hardwareRayTracingReflections;
    projectRenderingDraft_.hardwareRayTracingShadows = settings.hardwareRayTracingShadows;
    projectRenderingDraft_.globalIllumination = settings.globalIllumination;
    projectSettingsDirty_ = projectRenderingDraft_ != project_.rendering;
}

bool SceneEditor::saveProjectSettings() {
    std::string error;
    if (!project::saveProjectRenderingSettings(project_.manifest, projectRenderingDraft_, error)) {
        projectSettingsMessage_ = "Save failed: " + error;
        status_ = projectSettingsMessage_;
        return false;
    }
    project_.rendering = projectRenderingDraft_;
    projectSettingsDirty_ = false;
    projectSettingsMessage_ = "Project settings saved.";
    return true;
}

void SceneEditor::drawProjectMenu() {
    if (!ImGui::BeginMenu("Project")) return;
    if (ImGui::MenuItem("Project Settings...", nullptr, projectSettingsOpen_)) showProjectSettings();
    ImGui::EndMenu();
}

void SceneEditor::drawProjectSettings(renderer::SceneSettings& settings, renderer::SceneRenderer& renderer) {
    projectSettingsFocused_ = false;
    if (!projectSettingsOpen_) return;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720, 540), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(560, 390), ImVec2(FLT_MAX, FLT_MAX));
    if (projectSettingsFocusRequested_) {
        ImGui::SetNextWindowFocus();
        projectSettingsFocusRequested_ = false;
    }
    const auto flags = projectSettingsDirty_ ? ImGuiWindowFlags_UnsavedDocument : ImGuiWindowFlags_None;
    const bool visible = ImGui::Begin("Project Settings", &projectSettingsOpen_, flags);
    projectSettingsFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (visible) {
        ImGui::TextUnformatted(project_.name.c_str());
        ImGui::TextDisabled("Settings apply immediately. Save to keep them with this project.");
        ImGui::Separator();

        const float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * 2.0f;
        const float contentHeight = std::max(100.0f, ImGui::GetContentRegionAvail().y - footerHeight);
        ImGui::BeginChild("ProjectSettingsNavigation", ImVec2(135, contentHeight), ImGuiChildFlags_Borders);
        ImGui::TextDisabled("Categories");
        ImGui::Selectable("Rendering", true);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("ProjectSettingsContent", ImVec2(0, contentHeight), ImGuiChildFlags_Borders);
        ImGui::SeparatorText("Rendering");
        ImGui::TextWrapped("Configure project rendering features independently of the current device.");
        ImGui::Spacing();
        bool changed = ImGui::Checkbox("Hardware ray tracing", &settings.hardwareRayTracing);
        ImGui::TextWrapped("Trace the scene on hardware for ray-traced reflections and shadows.");
        ImGui::BeginDisabled(!settings.hardwareRayTracing);
        changed |= ImGui::Checkbox("Ray-traced reflections", &settings.hardwareRayTracingReflections);
        changed |= ImGui::Checkbox("Ray-traced shadows", &settings.hardwareRayTracingShadows);
        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::SeparatorText("Global illumination");
        auto& gi = settings.globalIllumination;
        int method = static_cast<int>(gi.method);
        if (ImGui::Combo("GI method", &method, "Disabled\0Progressive path tracing\0")) {
            gi.method = static_cast<renderer::GlobalIlluminationMethod>(method);
            if (gi.method == renderer::GlobalIlluminationMethod::PathTracing)
                settings.hardwareRayTracing = true;
            changed = true;
        }
        ImGui::TextWrapped("Selecting path tracing enables hardware ray tracing. Turning hardware off keeps your GI preference.");
        ImGui::BeginDisabled(gi.method != renderer::GlobalIlluminationMethod::PathTracing);
        const auto sampleSlider = [&changed](const char* label, std::uint32_t& value, std::uint32_t maximum) {
            int edited = static_cast<int>(std::clamp(value, 1u, maximum));
            if (ImGui::SliderInt(label, &edited, 1, static_cast<int>(maximum), "%d", ImGuiSliderFlags_AlwaysClamp)) {
                value = static_cast<std::uint32_t>(edited);
                changed = true;
            }
        };
        sampleSlider("Samples per frame", gi.samplesPerFrame, renderer::kGiMaxSamplesPerFrame);
        sampleSlider("Target samples", gi.targetSamples, renderer::kGiMaxTargetSamples);
        sampleSlider("Max bounces", gi.maxBounces, renderer::kGiMaxBounces);
        ImGui::EndDisabled();
        ImGui::TextWrapped("Changes update automatically. Keep the view still to accumulate a cleaner image; scene and lighting edits restart the affected history.");

        const auto giStatus = renderer.globalIlluminationStatus();
        const bool giEffective = giStatus.effective == renderer::GlobalIlluminationMethod::PathTracing;
        ImGui::Text("GI requested: %s", gi.method == renderer::GlobalIlluminationMethod::PathTracing
            ? "Progressive path tracing" : "Disabled");
        ImGui::Text("GI effective: %s", giEffective ? "Progressive path tracing" : "Disabled");
        if (giEffective) {
            const auto target = std::max(1u, giStatus.targetSamples);
            const auto progress = std::min(giStatus.accumulatedSamples, target);
            const std::string label = std::to_string(progress) + " / " + std::to_string(target) + " samples";
            ImGui::ProgressBar(static_cast<float>(progress) / static_cast<float>(target), ImVec2(-FLT_MIN, 0), label.c_str());
        }
        if (!giStatus.reason.empty()) ImGui::TextWrapped("%s", giStatus.reason.c_str());
        ImGui::BeginDisabled(!giEffective);
        if (ImGui::Button("Restart accumulation")) renderer.resetGlobalIllumination();
        ImGui::EndDisabled();
        if (changed) {
            syncProjectSettings(settings);
            projectSettingsMessage_.clear();
        }
        ImGui::Spacing();
        ImGui::SeparatorText("Device status");
        ImGui::Text("Support: %s", renderer.hardwareRayTracingSupported() ? "Available" : "Unavailable");
        ImGui::Text("Requested: %s", settings.hardwareRayTracing ? "Enabled" : "Disabled");
        ImGui::Text("Effective: %s", settings.hardwareRayTracing && renderer.hardwareRayTracingActive()
            ? "Active" : settings.hardwareRayTracing ? "Pending / unavailable" : "Disabled");
        ImGui::TextWrapped("%s", renderer.hardwareRayTracingStatus().c_str());
        ImGui::TextWrapped("Unavailable features retain your saved request and use the supported rendering path.");
        ImGui::Spacing();
        ImGui::SeparatorText("Compatibility preview");
        ImGui::BeginDisabled(settings.hardwareRayTracing);
        ImGui::Checkbox("Analytic reflections (DX11)", &settings.rayTracing);
        ImGui::EndDisabled();
        ImGui::TextWrapped("Session-only analytic tracing. Hardware ray tracing takes precedence when requested.");
        ImGui::EndChild();

        ImGui::Separator();
        ImGui::BeginDisabled(!projectSettingsDirty_);
        if (ImGui::Button("Save project settings")) saveProjectSettings();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", projectSettingsDirty_ ? "Unsaved changes" : "Saved with project");
        if (!projectSettingsMessage_.empty()) ImGui::TextWrapped("%s", projectSettingsMessage_.c_str());
    }
    ImGui::End();
}

} // namespace rae::editor
