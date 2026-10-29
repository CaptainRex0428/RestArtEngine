#pragma once

#include "RestArtEngine/Assets/AssetLibrary.h"
#include "RestArtEngine/Camera/Camera.h"
#include "RestArtEngine/Scene/SceneEnvironment.h"
#include "RestArtEngine/Scene/Light.h"
#include <optional>

namespace rae::scene {
using ActorId = std::uint64_t;
struct Transform {
    float position[3] = { 0, 0, 0 };
    float rotation[3] = { 0, 0, 0 }; // Euler degrees, X/Y/Z.
    float scale[3] = { 1, 1, 1 };
};
struct StaticMeshComponent {
    assets::AssetId meshId = 1;
    assets::AssetId materialId = 1;
    // Zero entries inherit mesh slot defaults, then the legacy Actor fallback.
    std::vector<assets::AssetId> materialOverrides;
    [[nodiscard]] assets::AssetId materialForSlot(const assets::MeshAsset& mesh, std::size_t slot) const noexcept {
        if (slot < materialOverrides.size() && materialOverrides[slot]) return materialOverrides[slot];
        if (slot < mesh.materialSlots.size() && mesh.materialSlots[slot].defaultMaterialId)
            return mesh.materialSlots[slot].defaultMaterialId;
        return materialId;
    }
};
struct Actor {
    ActorId id = 0;
    std::string name;
    Transform transform;
    StaticMeshComponent mesh;
    bool visible = true;
    // A light Actor has no mesh; its mesh IDs are zero.
    std::optional<LightComponent> light;
    // Camera Actors have no mesh or light. The Actor transform supplies the
    // position and Euler orientation; local -Z is forward, scale is ignored.
    std::optional<camera::Lens> camera;
};
class World {
public:
    std::vector<Actor> actors;
    // Scene-wide backdrop. false keeps the procedural sky gradient; true renders
    // a solid black background, which is what the ray-tracing test scene uses.
    bool blackBackground = false;
    SceneEnvironment environment;
    std::optional<SceneCamera> startupCamera;
    // Explicit authored output camera. Zero preserves the legacy opening view.
    // Editor navigation and the viewport's temporary binding do not change it.
    ActorId defaultCameraActor = 0;
    ActorId createActor(std::string name, assets::AssetId mesh, assets::AssetId material);
    ActorId createLight(std::string name, LightType type);
    ActorId createCamera(std::string name);
    bool removeActor(ActorId id);
    ActorId duplicateActor(ActorId id);
    Actor* findActor(ActorId id);
    const Actor* findActor(ActorId id) const;
    void createDefaultScene();
    // Same layout, but material references come from the loaded library so a
    // project with GUID materials still produces a valid scene.
    void createDefaultScene(const assets::AssetLibrary& library);
    // Writes Actor data and asset identities only. The caller checks that the
    // referenced assets are persisted; asset payloads are neither validated nor saved here.
    bool save(const std::filesystem::path& path, const assets::AssetLibrary& assets, std::string& error) const;
    bool load(const std::filesystem::path& path, assets::AssetLibrary& assets, std::string& error);
private:
    ActorId nextActorId_ = 1;
};
} // namespace rae::scene
