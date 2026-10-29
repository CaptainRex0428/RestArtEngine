#pragma once

#include <RestArtEngine/Renderer/SceneRenderer.h>
#include <RestArtEngine/Scene/World.h>
#include <RestArtEngine/Assets/AssetRegistry.h>
#include <RestArtEngine/Assets/MeshQuery.h>
#include <vector>
#include <functional>

namespace rae::editor {

// Viewport-only input and transform tools; runtime Actors never depend on ImGui.
class ViewportInteraction {
public:
    struct Surface {
        const char* name = "Viewport";
        float width = 1, height = 1;
        int index = 0;
        bool active = true;
        renderer::PreviewSlot previewSlot = renderer::PreviewSlot::Viewport0;
    };
    enum class CameraMode { Free, View, Pilot };
    bool setCameraView(renderer::SceneSettings&, const scene::World&, scene::ActorId, bool pilot);
    void returnToFreeCamera(renderer::SceneSettings&);
    // UI requests take effect before the next frame's image rectangle is built.
    void requestCameraView(scene::ActorId id, bool pilot) { pendingCameraAction_ = pilot ? 2 : 1; pendingCameraActor_ = id; }
    void requestReturnToFreeCamera() { pendingCameraAction_ = 3; }
    void requestFreeCamera(const renderer::SceneSettings&, bool restoreExposure = false);
    CameraMode cameraMode() const { return cameraMode_; }
    scene::ActorId viewedCamera() const { return cameraActor_; }
    bool isCameraNavigating() const { return orbiting_ || panning_ || cameraGesture_; }
    void stopCameraNavigation(const scene::World& world) { finishCameraGesture(world); orbiting_ = panning_ = false; }
    // Desktop image rectangle for diagnostics/interaction tests, refreshed each draw.
    const std::array<float, 4>& imageRect() const { return imageRect_; }
    // Reports one completed pilot gesture; scene editor owns its transaction.
    std::function<void(scene::ActorId, const scene::Transform&, const camera::Lens&,
                       const scene::Transform&, const camera::Lens&)> onCameraEdited;
    // selection is the full editor selection; selected is its primary Actor
    // (the one the gizmo pivots on). Picking replaces the selection.
    // duplicateForDrag copies that selection in place and selects its copies;
    // the editor owns creation/undo, while this class owns gesture arbitration.
    // A content drop stays a transient bounds preview until delivery; the
    // editor creates and records the final instance through createFromMesh.
    // keyboardBlocked is this frame's external tool keyboard ownership. Mouse
    // drags keep their existing camera / gizmo / asset ownership independently.
    // A minimized main surface does not hide an independent scene window.
    bool draw(renderer::SceneSettings&, scene::World&, const assets::AssetLibrary&,
              std::vector<scene::ActorId>& selection, scene::ActorId& selected,
              renderer::SceneRenderer& renderer, const std::function<void()>& duplicateForDrag,
              const assets::AssetRegistry& registry,
              const std::function<void(assets::AssetId, const scene::Transform&)>& createFromMesh,
              const std::function<void(scene::ActorId, assets::AssetId, std::size_t)>& assignMaterial,
              const assets::MaterialAsset* materialOverride = nullptr,
              bool keyboardBlocked = false, bool mainSurfaceVisible = true, const Surface* surface = nullptr);
    bool isUsing() const { return usingGizmo_; }
    bool isHovered() const { return hovered_; }
    bool isFocused() const { return focused_; }
    // True only for this draw's actual scene-picking path, including reselect.
    bool pickedThisFrame() const { return pickedThisFrame_; }
    // Transient drag feedback, independent of the editor's actual selection.
    bool materialDragging() const { return materialDragging_; }
    scene::ActorId materialDropTarget() const { return materialDropTarget_; }
private:
    int pendingCameraAction_ = 0;
    scene::ActorId pendingCameraActor_ = 0;
    camera::Camera pendingFreeCamera_;
    bool pendingFreeHadPose_ = false, pendingRestoreExposure_ = false;
    float pendingOrbit_[6]{}, pendingExposure_ = 1;
    std::array<float, 4> imageRect_{};
    CameraMode cameraMode_ = CameraMode::Free;
    scene::ActorId cameraActor_ = 0, gestureCamera_ = 0;
    camera::Camera savedFreeCamera_;
    float savedOrbit_[6]{};
    bool savedFreeHadPose_ = false, fly_ = false, cameraGesture_ = false;
    scene::Transform gestureTransform_;
    camera::Lens gestureLens_;
    float flySpeed_ = 3.0f;
    void finishCameraGesture(const scene::World&);
    std::unique_ptr<assets::MeshQueryCache> meshQueries_ = std::make_unique<assets::MeshQueryCache>();
    int operation_ = 0; // translate, rotate, scale
    bool local_ = false, snap_ = false;
    bool usingGizmo_ = false, hovered_ = false, focused_ = false, orbiting_ = false, panning_ = false;
    bool duplicateOnDrag_ = false;
    bool pickedThisFrame_ = false;
    bool materialDragging_ = false;
    scene::ActorId materialDropTarget_ = 0;
};

} // namespace rae::editor
