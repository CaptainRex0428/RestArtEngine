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
            ImGui::TextWrapped("DXR requires native D3D12 or D3D11 + D3D12 ray tracing. These controls are disabled for the selected next-launch backend.");
        else if (!renderer.hardwareRayTracingSupported())
            ImGui::TextWrapped("DXR is unavailable on the active adapter or pipeline. SSGI and screen-space probes can run in Deferred.");
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
        const bool realtimePathGi = gi.method == renderer::GlobalIlluminationMethod::PathTracing &&
            gi.mode == renderer::GlobalIlluminationMode::Realtime;
        const bool rasterGi = gi.method == renderer::GlobalIlluminationMethod::ScreenSpace ||
            gi.method == renderer::GlobalIlluminationMethod::ProbeGrid;
        const bool reflectionsEditable = hardware && (gi.method == renderer::GlobalIlluminationMethod::Disabled ||
            (rasterGi && gi.indirectSpecular));
        bool reflections = reflectionsEditable && settings.hardwareRayTracingReflections;
        ImGui::BeginDisabled(!reflectionsEditable);
        if (ImGui::Checkbox("Ray-traced reflections", &reflections)) {
            settings.hardwareRayTracingReflections = reflections;
            changed = true;
        }
        ImGui::EndDisabled();
        const bool shadowsEditable = hardware && !realtimePathGi;
        bool shadows = shadowsEditable && settings.hardwareRayTracingShadows;
        ImGui::BeginDisabled(!shadowsEditable);
        if (ImGui::Checkbox("Ray-traced shadows", &shadows)) {
            settings.hardwareRayTracingShadows = shadows;
            changed = true;
        }
        if (ImGui::IsItemHovered() && (gi.method == renderer::GlobalIlluminationMethod::ScreenSpace ||
                gi.method == renderer::GlobalIlluminationMethod::ProbeGrid))
            ImGui::SetTooltip("Controls DXR probe shadows. Secondary reflection hits test light visibility unless scene shadows are disabled.");
        ImGui::EndDisabled();
        if (rasterGi && gi.indirectSpecular)
            ImGui::TextWrapped("Ray-traced reflections include off-screen geometry. With this option off, reflections use screen-space tracing.");
        if (realtimePathGi)
            ImGui::TextWrapped("Realtime path tracing uses the selected Forward or Deferred direct-light shadows.");

        ImGui::Spacing();
        ImGui::SeparatorText("Global illumination");
        int method = static_cast<int>(gi.method);
        if (ImGui::Combo("GI method", &method, "Disabled\0Path tracing\0Screen space (SSGI)\0Probe grid (DDGI)\0")) {
            gi.method = static_cast<renderer::GlobalIlluminationMethod>(method);
            if (settings.debugView == 12 && gi.method != renderer::GlobalIlluminationMethod::ProbeGrid)
                settings.debugView = 0;
            if (settings.debugView == 11 &&
                (gi.method == renderer::GlobalIlluminationMethod::Disabled ||
                    (gi.method == renderer::GlobalIlluminationMethod::PathTracing &&
                        gi.mode == renderer::GlobalIlluminationMode::Progressive)))
                settings.debugView = 0;
            applyEdit(ProjectRenderingEdit::GiMethod);
        }
        const bool ssgiRequested = gi.method == renderer::GlobalIlluminationMethod::ScreenSpace;
        const bool ddgiRequested = gi.method == renderer::GlobalIlluminationMethod::ProbeGrid;
        const bool pathRequested = gi.method == renderer::GlobalIlluminationMethod::PathTracing;
        const auto giStatus = renderer.globalIlluminationStatus();
        const bool pathEffective = giStatus.effective == renderer::GlobalIlluminationMethod::PathTracing;
        const bool rasterGiEffective = giStatus.effective == renderer::GlobalIlluminationMethod::ScreenSpace ||
            giStatus.effective == renderer::GlobalIlluminationMethod::ProbeGrid;
        if (gi.method != renderer::GlobalIlluminationMethod::Disabled) {
            const bool active = giStatus.effective != renderer::GlobalIlluminationMethod::Disabled;
            ImGui::TextWrapped("Active in last scene frame: %s", active ? giExecutionLabel(giStatus) : "Inactive");
            if (rasterGiEffective) {
                if (hasActiveRasterGiQuality(giStatus))
                    ImGui::Text("Active quality: %s", giQualityLabel(giStatus.effectiveQuality));
                ImGui::Text("Scene reflections: %s", rasterReflectionLabel(giStatus));
            }
            if (!giStatus.reason.empty()) ImGui::TextWrapped("%s", giStatus.reason.c_str());
        }
        if (ssgiRequested || ddgiRequested) {
            if (settings.path != renderer::RenderPath::Deferred && ImGui::SmallButton("Use Deferred"))
                settings.path = renderer::RenderPath::Deferred;
            ImGui::TextUnformatted("Viewport output");
            ImGui::RadioButton("Lit", &settings.debugView, 0);
            ImGui::SameLine();
            ImGui::RadioButton("GI indirect", &settings.debugView, 11);
            if (ddgiRequested) {
                ImGui::SameLine();
                ImGui::RadioButton("GI probes", &settings.debugView, 12);
            }
            if (settings.debugView == 11) {
                ImGui::Combo("Indirect view EV", &settings.giDebugExposureEv, "0 EV\0+1 EV\0+2 EV\0+3 EV\0+4 EV\0");
                ImGui::SetItemTooltip("Brighten the isolated indirect-light view. Lit output and project lighting are unchanged.");
            }
        }
        const auto qualityControl = [&changed](const char* label, renderer::GlobalIlluminationQuality& quality) {
            int selected = static_cast<int>(quality);
            if (ImGui::Combo(label, &selected, "Custom\0Low\0Medium\0High\0")) {
                quality = static_cast<renderer::GlobalIlluminationQuality>(selected);
                changed = true;
            }
        };
        const auto countSlider = [&changed](const char* label, std::uint32_t& value, std::uint32_t maximum,
                                          std::uint32_t minimum = 1u) {
            int edited = static_cast<int>(std::clamp(value, minimum, maximum));
            if (ImGui::SliderInt(label, &edited, static_cast<int>(minimum), static_cast<int>(maximum),
                    "%d", ImGuiSliderFlags_AlwaysClamp)) {
                value = static_cast<std::uint32_t>(edited);
                changed = true;
            }
        };
        if (gi.method == renderer::GlobalIlluminationMethod::Disabled)
            ImGui::TextWrapped("GI is disabled. Hardware preview has no diffuse color bleeding.");
        if (ssgiRequested) {
            ImGui::TextWrapped("SSGI gathers bounced light from visible surfaces in Deferred.");
            qualityControl("SSGI quality", gi.screenSpaceQuality);
            const auto budget = renderer::resolveGiQuality(gi);
            if (gi.screenSpaceQuality == renderer::GlobalIlluminationQuality::Custom) {
                countSlider("SSGI diffuse rays", gi.screenSpaceRayCount, renderer::kGiMaxScreenSpaceRays);
                countSlider("SSGI steps", gi.screenSpaceSteps, renderer::kGiMaxScreenSpaceSteps);
            } else {
                ImGui::TextWrapped("Trace budget: %u diffuse rays per sample, up to %u steps per ray.",
                    budget.screenSpaceRayCount, budget.screenSpaceSteps);
            }
            changed |= ImGui::SliderFloat("SSGI radius", &gi.screenSpaceRadius, 0.05f, 20.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::SliderFloat("SSGI thickness", &gi.screenSpaceThickness, 0.01f, 2.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
        }
        if (ddgiRequested) {
            ImGui::TextWrapped("DDGI gathers diffuse bounce light in Deferred. DXR probes trace scene geometry. Screen-space probes sample surfaces visible to the camera.");
            qualityControl("DDGI quality", gi.probeQuality);
            const auto budget = renderer::resolveGiQuality(gi);
            if (gi.probeQuality == renderer::GlobalIlluminationQuality::Custom) {
                countSlider("Probe rays", gi.probeRayCount, renderer::kGiMaxProbeRays);
                countSlider("Screen-space probe steps", gi.probeScreenSpaceSteps, renderer::kGiMaxProbeScreenSpaceSteps);
                int shOrder = gi.probeShOrder > 1 ? 1 : 0;
                if (ImGui::Combo("Probe SH order", &shOrder, "SH-L1 (4 coefficients)\0SH-L2 (9 coefficients)\0")) {
                    gi.probeShOrder = static_cast<std::uint32_t>(shOrder + 1);
                    changed = true;
                }
                countSlider("Probe grid X", gi.probeGridX, renderer::kGiMaxProbeGridAxis, 2);
                countSlider("Probe grid Y", gi.probeGridY, renderer::kGiMaxProbeGridAxis, 2);
                countSlider("Probe grid Z", gi.probeGridZ, renderer::kGiMaxProbeGridAxis, 2);
            } else {
                ImGui::TextWrapped("%u x %u x %u grid, %u rays per probe, SH-L%u. Up to %u steps per screen-space ray.",
                    budget.probeGridX, budget.probeGridY, budget.probeGridZ, budget.probeRayCount,
                    budget.probeShOrder, budget.probeScreenSpaceSteps);
            }
            changed |= ImGui::Checkbox("Auto-fit probe grid to scene", &gi.probeAutoFit);
            ImGui::BeginDisabled(gi.probeAutoFit);
            changed |= ImGui::SliderFloat("Probe spacing", &gi.probeSpacing, 0.1f,
                renderer::kGiMaxProbeSpacing, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
            ImGui::EndDisabled();
            changed |= ImGui::SliderFloat("Probe hysteresis", &gi.probeHysteresis, 0.0f, 1.0f,
                "%.2f", ImGuiSliderFlags_AlwaysClamp);
        }
        if (pathRequested) {
            int mode = static_cast<int>(gi.mode);
            if (ImGui::Combo("GI mode", &mode, "Progressive accumulation\0Realtime\0")) {
                gi.mode = static_cast<renderer::GlobalIlluminationMode>(mode);
                if (settings.debugView == 11 && gi.mode == renderer::GlobalIlluminationMode::Progressive)
                    settings.debugView = 0;
                applyEdit(ProjectRenderingEdit::GiMode);
            }
        }
        if (gi.method != renderer::GlobalIlluminationMethod::Disabled) {
            changed |= ImGui::Checkbox("Indirect diffuse (color bleeding)", &gi.indirectDiffuse);
            ImGui::BeginDisabled(!gi.indirectDiffuse);
            ImGui::TextUnformatted("Indirect diffuse intensity");
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::SliderFloat("##IndirectDiffuseIntensity", &gi.indirectDiffuseIntensity,
                0.0f, renderer::kGiMaxIndirectDiffuseIntensity, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Values above 1x amplify diffuse color bleeding.");
            ImGui::EndDisabled();
            if (!gi.indirectDiffuse)
                ImGui::TextWrapped("Indirect diffuse is off. Diffuse color bleeding is disabled at the receiving surface.");
            changed |= ImGui::Checkbox("Indirect specular (reflections)", &gi.indirectSpecular);
            if (ddgiRequested && gi.indirectSpecular &&
                (!hardware || !settings.hardwareRayTracingReflections || giStatus.usesScreenSpaceReflections)) {
                changed |= ImGui::SliderFloat("SSR distance", &gi.screenSpaceRadius, 0.05f, 20.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
                countSlider("SSR steps", gi.screenSpaceSteps, renderer::kGiMaxScreenSpaceSteps);
                changed |= ImGui::SliderFloat("SSR thickness", &gi.screenSpaceThickness, 0.01f, 2.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
            }
        }
        const bool realtimeRequested = gi.mode == renderer::GlobalIlluminationMode::Realtime;
        if (pathRequested) {
            countSlider("Samples per frame", gi.samplesPerFrame, renderer::kGiMaxSamplesPerFrame);
            countSlider(realtimeRequested ? "History samples" : "Target samples", gi.targetSamples,
                renderer::kGiMaxTargetSamples);
            countSlider("Max bounces", gi.maxBounces, renderer::kGiMaxBounces);
        }
        if (pathRequested && realtimeRequested) {
            int sampling = static_cast<int>(gi.sampling);
            if (ImGui::Combo("Sampling", &sampling, "Uniform (all lights)\0ReSTIR reservoirs\0")) {
                gi.sampling = static_cast<renderer::GlobalIlluminationSampling>(sampling);
                changed = true;
            }
            if (gi.sampling == renderer::GlobalIlluminationSampling::Restir) {
                countSlider("Reservoir candidates", gi.restirCandidates, renderer::kGiMaxRestirCandidates);
                changed |= ImGui::Checkbox("ReSTIR direct", &gi.restirDirect);
                changed |= ImGui::Checkbox("ReSTIR indirect", &gi.restirIndirect);
                changed |= ImGui::Checkbox("ReSTIR spatial reuse", &gi.restirSpatialReuse);
            }
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
            countSlider("Denoise iterations", gi.denoiseIterations, renderer::kGiMaxDenoiseIterations);
            ImGui::TextWrapped("Realtime GI updates continuously. Its history settings control how quickly the lighting responds to changes.");
        }
        if (pathRequested && pathEffective) {
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
        }
        if (pathRequested && pathEffective && giStatus.effectiveMode == renderer::GlobalIlluminationMode::Realtime) {
            const char* denoiser = giStatus.effectiveDenoiser == renderer::GlobalIlluminationDenoiser::TemporalSpatial
                ? "Temporal + spatial" : giStatus.effectiveDenoiser == renderer::GlobalIlluminationDenoiser::Spatial
                    ? "Spatial (5x5)" : "Off";
            const char* motion = giStatus.effectiveMotionVectors == renderer::GlobalIlluminationMotionVectors::PerObject
                ? "Per object" : giStatus.effectiveMotionVectors == renderer::GlobalIlluminationMotionVectors::CameraOnly
                    ? "Camera only" : "Automatic";
            ImGui::Text("GI denoiser: %s", denoiser);
            ImGui::Text("GI motion vectors: %s", motion);
        }
        if (gi.method != renderer::GlobalIlluminationMethod::Disabled) {
            ImGui::BeginDisabled(!pathEffective && !rasterGiEffective);
            const char* resetLabel = pathRequested ? "Restart accumulation" : "Reset GI history";
            if (ImGui::Button(resetLabel)) renderer.resetGlobalIllumination();
            ImGui::EndDisabled();
        }
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
