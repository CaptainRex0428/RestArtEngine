#include <RestArtEngine/Editor/SceneEditor.h>
#include <RestArtEngine/Editor/ProjectSettingsRules.h>
#include <imgui.h>

#include <algorithm>
#include <cfloat>

namespace rae::editor {

void SceneEditor::syncProjectSettings(const renderer::SceneSettings& settings) {
    projectRenderingDraft_.hardwareRayTracing = settings.hardwareRayTracing;
    projectRenderingDraft_.hardwareRayTracingReflections = settings.hardwareRayTracingReflections;
    projectRenderingDraft_.hardwareRayTracingShadows = settings.hardwareRayTracingShadows;
    projectRenderingDraft_.globalIllumination = settings.globalIllumination;
    projectRenderingDraft_.antiAliasing = settings.antiAliasing;
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
        ImGui::TextWrapped("Supported rendering changes update immediately. Backend changes require saving and restarting the editor.");
        ImGui::Separator();

        const bool startupNotice = projectRestartNotice(projectRenderingDraft_, project_.rendering,
            renderer.mainBackendSelection().requested) != ProjectRestartNotice::None;
        const float footerHeight = ImGui::GetFrameHeightWithSpacing() +
            ImGui::GetTextLineHeightWithSpacing() * (startupNotice ? 6.0f : 2.0f);
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
        bool changed = false;
        const auto applyEdit = [&](ProjectRenderingEdit edit) {
            syncProjectSettings(settings);
            applyProjectRenderingEdit(projectRenderingDraft_, edit);
            settings.hardwareRayTracing = projectRenderingDraft_.hardwareRayTracing;
            settings.hardwareRayTracingReflections = projectRenderingDraft_.hardwareRayTracingReflections;
            settings.hardwareRayTracingShadows = projectRenderingDraft_.hardwareRayTracingShadows;
            settings.globalIllumination = projectRenderingDraft_.globalIllumination;
            if (settings.hardwareRayTracing || projectRenderingDraft_.backendPreference == renderer::MainRenderBackend::D3D12)
                settings.rayTracing = false;
            changed = true;
        };
        int backend = static_cast<int>(projectRenderingDraft_.backendPreference);
        ImGui::TextUnformatted("Main backend (next launch)");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##MainBackend", &backend,
            "D3D11 + D3D12 ray tracing\0Auto\0D3D11 (strict)\0D3D12 (native DXR)\0")) {
            projectRenderingDraft_.backendPreference = static_cast<renderer::MainRenderBackend>(backend);
            applyEdit(ProjectRenderingEdit::Backend);
        }
        const bool fallbackEditable = projectRenderingDraft_.backendPreference == renderer::MainRenderBackend::D3D12;
        bool fallback = fallbackEditable && projectRenderingDraft_.allowFallbackToD3D11;
        ImGui::BeginDisabled(!fallbackEditable);
        if (ImGui::Checkbox("Allow fallback to D3D11", &fallback)) {
            projectRenderingDraft_.allowFallbackToD3D11 = fallback;
            changed = true;
        }
        ImGui::EndDisabled();
        if (!fallbackEditable) ImGui::TextDisabled("Fallback applies only to the native D3D12 backend.");
        const auto& activeBackend = renderer.mainBackendSelection();
        ImGui::Text("Current main backend: %s", renderer.mainBackendReady()
            ? renderer::mainRenderBackendKey(activeBackend.effective) : "Not initialized");
        ImGui::Text("This launch requested: %s", renderer::mainRenderBackendKey(activeBackend.requested.preference));
        ImGui::TextWrapped("%s", activeBackend.reason);
        if (!renderer.backendInitializationError().empty())
            ImGui::TextWrapped("D3D12 initialization failed: %s", renderer.backendInitializationError().c_str());
        ImGui::TextWrapped("Auto uses strict D3D11. Native D3D12 executes native DXR on capable adapters (experimental raster otherwise). Hybrid mode uses D3D11 raster and optional D3D12/DXR ray tracing.");
        if (activeBackend.effective == renderer::MainRenderBackend::D3D12)
            ImGui::TextWrapped("Native D3D12 executes DXR ray tracing/GI when the device supports DXR 1.1; depth of field and selection outlines remain raster-only for now.");
        ImGui::Spacing();
        ImGui::SeparatorText("Anti-aliasing");
        int aaMethod = static_cast<int>(settings.antiAliasing.method);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##AntiAliasingMethod", &aaMethod, "Off\0FXAA\0TAA\0")) {
            settings.antiAliasing.method = static_cast<renderer::AntiAliasingMethod>(aaMethod);
            changed = true;
        }
        ImGui::TextWrapped("FXAA smooths edges in the current image. TAA combines jittered frames for steadier subpixel detail.");
        if (settings.antiAliasing.method == renderer::AntiAliasingMethod::TAA) {
            ImGui::TextUnformatted("Current frame weight");
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::SliderFloat("##TemporalCurrentWeight", &settings.antiAliasing.temporalCurrentWeight,
                .02f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::TextWrapped("Lower weights favor a stable image; higher weights respond faster to motion and lighting changes.");
        }
        const auto aaStatus = renderer.antiAliasingStatus();
        ImGui::Text("Requested: %s", renderer::antiAliasingMethodName(settings.antiAliasing.method));
        ImGui::Text("Effective: %s", renderer::antiAliasingMethodName(aaStatus.effective));
        if (aaStatus.effective == renderer::AntiAliasingMethod::TAA)
            ImGui::Text("History: %u frames", aaStatus.accumulatedFrames);
        if (!aaStatus.reason.empty()) ImGui::TextWrapped("%s", aaStatus.reason.c_str());
        ImGui::Spacing();
        const bool hardwareEditable = projectBackendAllowsHardware(projectRenderingDraft_.backendPreference);
        if (!hardwareEditable)
            ImGui::TextWrapped("Hardware ray tracing and path-traced GI require the native D3D12 or D3D11 + D3D12 ray tracing backend. These controls are disabled for the selected next-launch backend.");
        else if (!renderer.hardwareRayTracingSupported())
            ImGui::TextWrapped("DXR is unavailable on the active adapter or pipeline; lighting requests are retained but inactive.");
        else if (projectRenderingDraft_.backendPreference != activeBackend.requested.preference)
            ImGui::TextWrapped("Requests apply live after restarting into the selected backend.");
        ImGui::Spacing();
        ImGui::SeparatorText("Lighting features");
        auto& gi = settings.globalIllumination;
        bool hardware = hardwareEditable && settings.hardwareRayTracing;
        ImGui::BeginDisabled(!hardwareEditable);
        if (ImGui::Checkbox("Hardware ray tracing", &hardware)) {
            settings.hardwareRayTracing = hardware;
            applyEdit(ProjectRenderingEdit::HardwareRayTracing);
        }
        ImGui::EndDisabled();
        ImGui::TextWrapped("Enabling hardware tracing selects reflections and shadows. Disabling it also disables dependent effects and GI.");
        const bool giEnabled = gi.method != renderer::GlobalIlluminationMethod::Disabled;
        const bool realtimeGi = giEnabled && gi.mode == renderer::GlobalIlluminationMode::Realtime;
        const bool reflectionsEditable = hardware && !giEnabled;
        bool reflections = reflectionsEditable && settings.hardwareRayTracingReflections;
        ImGui::BeginDisabled(!reflectionsEditable);
        if (ImGui::Checkbox("Ray-traced reflections", &reflections)) {
            settings.hardwareRayTracingReflections = reflections;
            changed = true;
        }
        ImGui::EndDisabled();
        const bool shadowsEditable = hardware && !realtimeGi;
        bool shadows = shadowsEditable && settings.hardwareRayTracingShadows;
        ImGui::BeginDisabled(!shadowsEditable);
        if (ImGui::Checkbox("Ray-traced shadows", &shadows)) {
            settings.hardwareRayTracingShadows = shadows;
            changed = true;
        }
        ImGui::EndDisabled();
        if (giEnabled)
            ImGui::TextWrapped("GI uses indirect specular below. Selecting path tracing clears standalone ray-traced reflections.");
        if (realtimeGi)
            ImGui::TextWrapped("Realtime GI uses Forward / Deferred direct-light shadows and clears standalone ray-traced shadows.");
        if ((!hardwareEditable && (settings.hardwareRayTracing || giEnabled)) ||
            (!settings.hardwareRayTracing && (settings.hardwareRayTracingReflections || settings.hardwareRayTracingShadows || giEnabled)))
            ImGui::TextWrapped("Inactive requests loaded from the project are retained. Explicit parent changes clear dependent requests; device limitations alone do not rewrite project settings.");

        ImGui::Spacing();
        ImGui::SeparatorText("Global illumination");
        int method = static_cast<int>(gi.method);
        if (ImGui::Combo("GI method", &method, "Disabled\0Path tracing\0Screen space (SSGI)\0Probe grid (DDGI)\0")) {
            gi.method = static_cast<renderer::GlobalIlluminationMethod>(method);
            applyEdit(ProjectRenderingEdit::GiMethod);
        }
        if (gi.method == renderer::GlobalIlluminationMethod::Disabled)
            ImGui::TextWrapped("GI is disabled. Hardware preview has no diffuse color bleeding.");
        if (gi.method == renderer::GlobalIlluminationMethod::ScreenSpace) {
            ImGui::TextWrapped("SSGI traces the current depth/normal buffer and samples last frame's lit color. It needs the Deferred path; Forward reports it unavailable. DXR hardware is not required.");
            int steps = static_cast<int>(gi.screenSpaceSteps);
            if (ImGui::SliderInt("SSGI steps", &steps, 1, static_cast<int>(renderer::kGiMaxScreenSpaceSteps),
                    "%d", ImGuiSliderFlags_AlwaysClamp)) {
                gi.screenSpaceSteps = static_cast<std::uint32_t>(steps);
                changed = true;
            }
            changed |= ImGui::SliderFloat("SSGI radius", &gi.screenSpaceRadius, 0.05f, 20.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::SliderFloat("SSGI thickness", &gi.screenSpaceThickness, 0.01f, 2.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
        }
        if (gi.method == renderer::GlobalIlluminationMethod::ProbeGrid) {
            ImGui::TextWrapped("DDGI fits a probe grid to the scene bounds (or cameraTarget when auto-fit is off). DXR backends ray-trace each probe; strict D3D11 falls back to a screen-space update. Probe rays controls the per-probe ray budget.");
            changed |= ImGui::Checkbox("Auto-fit probe grid to scene", &gi.probeAutoFit);
            int rays = static_cast<int>(gi.probeRayCount);
            if (ImGui::SliderInt("Probe rays", &rays, 1, static_cast<int>(renderer::kGiMaxProbeRays),
                    "%d", ImGuiSliderFlags_AlwaysClamp)) {
                gi.probeRayCount = static_cast<std::uint32_t>(rays);
                changed = true;
            }
            int x = static_cast<int>(gi.probeGridX), y = static_cast<int>(gi.probeGridY), z = static_cast<int>(gi.probeGridZ);
            const int axisMax = static_cast<int>(renderer::kGiMaxProbeGridAxis);
            if (ImGui::SliderInt("Probe grid X", &x, 2, axisMax, "%d", ImGuiSliderFlags_AlwaysClamp)) {
                gi.probeGridX = static_cast<std::uint32_t>(x); changed = true;
            }
            if (ImGui::SliderInt("Probe grid Y", &y, 2, axisMax, "%d", ImGuiSliderFlags_AlwaysClamp)) {
                gi.probeGridY = static_cast<std::uint32_t>(y); changed = true;
            }
            if (ImGui::SliderInt("Probe grid Z", &z, 2, axisMax, "%d", ImGuiSliderFlags_AlwaysClamp)) {
                gi.probeGridZ = static_cast<std::uint32_t>(z); changed = true;
            }
            ImGui::BeginDisabled(gi.probeAutoFit);
            changed |= ImGui::SliderFloat("Probe spacing", &gi.probeSpacing, 0.1f,
                renderer::kGiMaxProbeSpacing, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
            ImGui::EndDisabled();
            changed |= ImGui::SliderFloat("Probe hysteresis", &gi.probeHysteresis, 0.0f, 1.0f,
                "%.2f", ImGuiSliderFlags_AlwaysClamp);
        }
        ImGui::TextWrapped("Selecting path tracing enables hardware ray tracing and disables the competing standalone reflections.");
        ImGui::BeginDisabled(gi.method != renderer::GlobalIlluminationMethod::PathTracing);
        int mode = static_cast<int>(gi.mode);
        if (ImGui::Combo("GI mode", &mode, "Progressive accumulation\0Realtime\0")) {
            gi.mode = static_cast<renderer::GlobalIlluminationMode>(mode);
            applyEdit(ProjectRenderingEdit::GiMode);
        }
        ImGui::EndDisabled();
        changed |= ImGui::Checkbox("Indirect diffuse (color bleeding)", &gi.indirectDiffuse);
        ImGui::BeginDisabled(!gi.indirectDiffuse);
        ImGui::TextUnformatted("Indirect diffuse intensity");
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::SliderFloat("##IndirectDiffuseIntensity", &gi.indirectDiffuseIntensity,
            0.0f, renderer::kGiMaxIndirectDiffuseIntensity, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
        ImGui::EndDisabled();
        ImGui::TextWrapped("1x keeps the reference lighting. Above 1x boosts color bleeding for artistic control; direct light and specular reflections are unchanged.");
        if (gi.method != renderer::GlobalIlluminationMethod::Disabled && !gi.indirectDiffuse)
            ImGui::TextWrapped("Indirect diffuse is off. Diffuse color bleeding is disabled at the receiving surface.");
        changed |= ImGui::Checkbox("Indirect specular (reflections)", &gi.indirectSpecular);
        const auto sampleSlider = [&changed](const char* label, std::uint32_t& value, std::uint32_t maximum) {
            int edited = static_cast<int>(std::clamp(value, 1u, maximum));
            if (ImGui::SliderInt(label, &edited, 1, static_cast<int>(maximum), "%d", ImGuiSliderFlags_AlwaysClamp)) {
                value = static_cast<std::uint32_t>(edited);
                changed = true;
            }
        };
        ImGui::BeginDisabled(gi.method != renderer::GlobalIlluminationMethod::PathTracing);
        sampleSlider("Samples per frame", gi.samplesPerFrame, renderer::kGiMaxSamplesPerFrame);
        const bool realtimeRequested = gi.mode == renderer::GlobalIlluminationMode::Realtime;
        sampleSlider(realtimeRequested ? "History samples" : "Target samples", gi.targetSamples, renderer::kGiMaxTargetSamples);
        sampleSlider("Max bounces", gi.maxBounces, renderer::kGiMaxBounces);
        ImGui::EndDisabled();
        const bool realtimePath = gi.method == renderer::GlobalIlluminationMethod::PathTracing && realtimeRequested;
        const bool ssgiRequested = gi.method == renderer::GlobalIlluminationMethod::ScreenSpace;
        const bool ddgiRequested = gi.method == renderer::GlobalIlluminationMethod::ProbeGrid;
        ImGui::BeginDisabled(!realtimePath);
        int sampling = static_cast<int>(gi.sampling);
        if (ImGui::Combo("Sampling", &sampling, "Uniform (all lights)\0ReSTIR reservoirs\0")) {
            gi.sampling = static_cast<renderer::GlobalIlluminationSampling>(sampling);
            changed = true;
        }
        ImGui::BeginDisabled(gi.sampling != renderer::GlobalIlluminationSampling::Restir);
        int candidates = static_cast<int>(std::clamp(gi.restirCandidates, 1u, renderer::kGiMaxRestirCandidates));
        if (ImGui::SliderInt("Reservoir candidates", &candidates, 1,
                static_cast<int>(renderer::kGiMaxRestirCandidates), "%d", ImGuiSliderFlags_AlwaysClamp)) {
            gi.restirCandidates = static_cast<std::uint32_t>(candidates);
            changed = true;
        }
        changed |= ImGui::Checkbox("ReSTIR direct", &gi.restirDirect);
        changed |= ImGui::Checkbox("ReSTIR indirect", &gi.restirIndirect);
        changed |= ImGui::Checkbox("ReSTIR spatial reuse", &gi.restirSpatialReuse);
        ImGui::EndDisabled();
        int denoiser = static_cast<int>(gi.denoiser);
        if (ImGui::Combo("Denoiser", &denoiser, "Off\0Spatial (5x5)\0Temporal + spatial\0")) {
            gi.denoiser = static_cast<renderer::GlobalIlluminationDenoiser>(denoiser);
            changed = true;
        }
        int motionVectors = static_cast<int>(gi.motionVectors);
        if (ImGui::Combo("Motion vectors", &motionVectors, "Camera only\0Per object\0Automatic\0")) {
            gi.motionVectors = static_cast<renderer::GlobalIlluminationMotionVectors>(motionVectors);
            changed = true;
        }
        int denoiseIterations = static_cast<int>(std::clamp(gi.denoiseIterations, 1u, renderer::kGiMaxDenoiseIterations));
        if (ImGui::SliderInt("Denoise iterations", &denoiseIterations, 1,
                static_cast<int>(renderer::kGiMaxDenoiseIterations), "%d", ImGuiSliderFlags_AlwaysClamp)) {
            gi.denoiseIterations = static_cast<std::uint32_t>(denoiseIterations);
            changed = true;
        }
        ImGui::EndDisabled();
        if (realtimePath && gi.denoiser == renderer::GlobalIlluminationDenoiser::TemporalSpatial)
            ImGui::TextWrapped("Temporal denoising reprojects history with camera and per-object motion vectors.");
        if (realtimePath && gi.sampling == renderer::GlobalIlluminationSampling::Restir)
            ImGui::TextWrapped("ReSTIR uses weighted reservoir sampling over light/path candidates. Direct and indirect toggles select the layers; temporal stability still comes from the GI history, not a separate reservoir history.");
        if (ssgiRequested)
            ImGui::TextWrapped("SSGI runs as a single deferred pass against last frame's lit color; camera motion keeps one frame of history lag. It does not use path samples, denoiser settings or hardware ray tracing.");
        if (realtimeRequested || ssgiRequested)
            ImGui::TextWrapped("Realtime GI updates continuously. Temporal denoising reprojects history with camera motion and per-object motion vectors; scene/lighting/material changes reset the reconstruction history.");
        else
            ImGui::TextWrapped("Progressive accumulation stops at the target sample count. Keep the view still for a cleaner image; scene and lighting edits automatically restart the affected history.");

        const auto giStatus = renderer.globalIlluminationStatus();
        const bool pathEffective = giStatus.effective == renderer::GlobalIlluminationMethod::PathTracing;
        const bool ssgiEffective = giStatus.effective == renderer::GlobalIlluminationMethod::ScreenSpace;
        const bool ddgiEffective = giStatus.effective == renderer::GlobalIlluminationMethod::ProbeGrid;
        const bool giEffective = pathEffective || ssgiEffective || ddgiEffective;
        const auto methodName = [](renderer::GlobalIlluminationMethod value) {
            return value == renderer::GlobalIlluminationMethod::PathTracing ? "Path tracing"
                : value == renderer::GlobalIlluminationMethod::ScreenSpace ? "Screen space (SSGI)"
                : value == renderer::GlobalIlluminationMethod::ProbeGrid ? "Probe grid (DDGI)" : "Disabled";
        };
        ImGui::Text("GI requested: %s", methodName(gi.method));
        ImGui::Text("GI requested mode: %s", (ssgiRequested || ddgiRequested) ? "Realtime (raster pass)"
            : realtimeRequested ? "Realtime" : "Progressive accumulation");
        ImGui::Text("GI effective: %s", methodName(giStatus.effective));
        if (pathEffective) {
            const auto target = std::max(1u, giStatus.targetSamples);
            const auto progress = std::min(giStatus.accumulatedSamples, target);
            if (giStatus.effectiveMode == renderer::GlobalIlluminationMode::Realtime) {
                ImGui::TextUnformatted("GI effective mode: Realtime (continuous updates)");
                ImGui::Text("History: %u / %u samples", progress, target);
            } else {
                ImGui::TextUnformatted("GI effective mode: Progressive accumulation");
                const std::string label = std::to_string(progress) + " / " + std::to_string(target) + " samples";
                ImGui::ProgressBar(static_cast<float>(progress) / static_cast<float>(target), ImVec2(-FLT_MIN, 0), label.c_str());
            }
        } else if (ssgiEffective) {
            ImGui::TextUnformatted("GI effective mode: Realtime deferred SSGI (previous-frame color)");
        } else if (ddgiEffective) {
            ImGui::TextUnformatted("GI effective mode: Realtime DDGI probe atlas (previous-frame update)");
        }
        if (pathEffective && giStatus.effectiveMode == renderer::GlobalIlluminationMode::Realtime) {
            const char* denoiser = giStatus.effectiveDenoiser == renderer::GlobalIlluminationDenoiser::TemporalSpatial
                ? "Temporal + spatial" : giStatus.effectiveDenoiser == renderer::GlobalIlluminationDenoiser::Spatial
                    ? "Spatial (5x5)" : "Off";
            const char* motion = giStatus.effectiveMotionVectors == renderer::GlobalIlluminationMotionVectors::PerObject
                ? "Per object" : giStatus.effectiveMotionVectors == renderer::GlobalIlluminationMotionVectors::CameraOnly
                    ? "Camera only" : "Automatic";
            ImGui::Text("GI denoiser: %s", denoiser);
            ImGui::Text("GI motion vectors: %s", motion);
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
        const bool analyticEditable = !settings.hardwareRayTracing &&
            projectRenderingDraft_.backendPreference != renderer::MainRenderBackend::D3D12 &&
            activeBackend.effective != renderer::MainRenderBackend::D3D12;
        bool analytic = analyticEditable && settings.rayTracing;
        ImGui::BeginDisabled(!analyticEditable);
        if (ImGui::Checkbox("Analytic reflections (D3D11)", &analytic)) settings.rayTracing = analytic;
        ImGui::EndDisabled();
        ImGui::TextWrapped("Session-only D3D11 analytic tracing. Enabling hardware tracing clears this competing option.");
        ImGui::EndChild();

        ImGui::Separator();
        const auto restart = projectRestartNotice(projectRenderingDraft_, project_.rendering,
            renderer.mainBackendSelection().requested);
        if (restart == ProjectRestartNotice::SaveAndRestart)
            ImGui::TextWrapped("Restart required: save project settings, then restart the editor.");
        else if (restart == ProjectRestartNotice::Restart)
            ImGui::TextWrapped("Restart required: settings are saved. Restart the editor to apply the startup configuration.");
        else if (restart == ProjectRestartNotice::SaveToCancelRestart)
            ImGui::TextWrapped("Save to cancel restart: the saved startup configuration still differs from this session.");
        if (restart != ProjectRestartNotice::None)
            ImGui::TextWrapped("Launch without backend command-line overrides to use the saved project configuration.");
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
