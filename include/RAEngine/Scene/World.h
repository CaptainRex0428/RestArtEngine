#pragma once

#include "RAEngine/Assets/AssetLibrary.h"

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
};
struct Actor {
    ActorId id = 0;
    std::string name;
    Transform transform;
    StaticMeshComponent mesh;
    bool visible = true;
};
class World {
public:
    std::vector<Actor> actors;
    ActorId createActor(std::string name, assets::AssetId mesh, assets::AssetId material);
    bool removeActor(ActorId id);
    ActorId duplicateActor(ActorId id);
    Actor* findActor(ActorId id);
    const Actor* findActor(ActorId id) const;
    void createDefaultScene();
    bool save(const std::filesystem::path& path, const assets::AssetLibrary& assets, std::string& error) const;
    bool load(const std::filesystem::path& path, assets::AssetLibrary& assets, std::string& error);
private:
    ActorId nextActorId_ = 1;
};
} // namespace rae::scene
