#include "RestArtEngine/Scene/World.h"
#include "../Assets/AssetSerialization.h"
#include <algorithm>
#include <cmath>

namespace rae::scene {
namespace {
using assets::detail::Json;
using assets::detail::readId;
void bounded(float value, float low, float high, const char* name) {
    if (!std::isfinite(value) || value < low || value > high)
        throw std::runtime_error(std::string("Invalid scene ") + name + ".");
}
float readNumber(const Json& value, const char* name) {
    if (!value.is_number()) throw std::runtime_error(std::string("Scene ") + name + " must be numeric.");
    return value.get<float>();
}
void readVector(const Json& value, float (&destination)[3], const char* name) {
    if (!value.is_array() || value.size() != 3)
        throw std::runtime_error(std::string("Scene ") + name + " must contain three numbers.");
    for (int axis = 0; axis < 3; ++axis) destination[axis] = readNumber(value[axis], name);
}
void validatePresentation(const World& world) {
    if (world.startupCamera) {
        const auto& camera = *world.startupCamera;
        bounded(camera.yaw, -1000000, 1000000, "camera yaw");
        bounded(camera.pitch, -1.55f, 1.55f, "camera pitch");
        bounded(camera.distance, 2, 30, "camera distance");
        bounded(camera.exposure, .01f, 64, "camera exposure");
        for (float value : camera.target) bounded(value, -1000000, 1000000, "camera target");
    }
    if (!world.environment.overrideLighting) return;
    const auto& environment = world.environment;
    bounded(environment.directionalIntensity, 0, 100000, "directional intensity");
    bounded(environment.environmentIntensity, 0, 10000, "environment intensity");
    const auto& light = environment.areaLight;
    double lengthU = 0, lengthV = 0, product = 0;
    for (int axis = 0; axis < 3; ++axis) {
        bounded(light.center[axis], -1000000, 1000000, "area light center");
        bounded(light.radiance[axis], 0, 1000000, "area light radiance");
        bounded(light.halfAxisU[axis], -10000, 10000, "area light axis U");
        bounded(light.halfAxisV[axis], -10000, 10000, "area light axis V");
        lengthU += double(light.halfAxisU[axis]) * light.halfAxisU[axis];
        lengthV += double(light.halfAxisV[axis]) * light.halfAxisV[axis];
        product += double(light.halfAxisU[axis]) * light.halfAxisV[axis];
    }
    if (lengthU < 1e-8 || lengthV < 1e-8 || std::abs(product) > 1e-4 * std::sqrt(lengthU * lengthV))
        throw std::runtime_error("Scene area light axes must be nonzero and perpendicular.");
}
void readPresentation(const Json& value, World& world) {
    if (value.contains("camera")) {
        const auto& input = value.at("camera");
        if (!input.is_object()) throw std::runtime_error("Scene camera must be an object.");
        SceneCamera camera;
        camera.yaw = readNumber(input.at("yaw"), "camera yaw");
        camera.pitch = readNumber(input.at("pitch"), "camera pitch");
        camera.distance = readNumber(input.at("distance"), "camera distance");
        readVector(input.at("target"), camera.target, "camera target");
        camera.exposure = readNumber(input.at("exposure"), "camera exposure");
        world.startupCamera = camera;
    }
    if (value.contains("lighting")) {
        const auto& input = value.at("lighting");
        if (!input.is_object()) throw std::runtime_error("Scene lighting must be an object.");
        auto& environment = world.environment;
        environment.overrideLighting = true;
        if (input.contains("directionalIntensity"))
            environment.directionalIntensity = readNumber(input.at("directionalIntensity"), "directional intensity");
        if (input.contains("environmentIntensity"))
            environment.environmentIntensity = readNumber(input.at("environmentIntensity"), "environment intensity");
        if (input.contains("areaLight")) {
            const auto& source = input.at("areaLight");
            if (!source.is_object()) throw std::runtime_error("Scene area light must be an object.");
            auto& light = environment.areaLight;
            if (!source.at("enabled").is_boolean()) throw std::runtime_error("Scene area light enabled must be boolean.");
            light.enabled = source.at("enabled").get<bool>();
            readVector(source.at("center"), light.center, "area light center");
            readVector(source.at("halfAxisU"), light.halfAxisU, "area light axis U");
            readVector(source.at("halfAxisV"), light.halfAxisV, "area light axis V");
            readVector(source.at("radiance"), light.radiance, "area light radiance");
        }
    }
    validatePresentation(world);
}
void validateActor(const Actor& actor, const assets::AssetLibrary& library) {
    readId(Json(actor.id));
    if (actor.name.empty()) throw std::runtime_error("Actor name cannot be empty.");
    if (!library.findMesh(actor.mesh.meshId)) throw std::runtime_error("Actor '"+actor.name+"' references a missing mesh.");
    if (!library.findMaterial(actor.mesh.materialId)) throw std::runtime_error("Actor '"+actor.name+"' references a missing material.");
    for (int axis=0;axis<3;++axis) {
        if (!std::isfinite(actor.transform.position[axis]) || !std::isfinite(actor.transform.rotation[axis]) ||
            !std::isfinite(actor.transform.scale[axis])) throw std::runtime_error("Actor transform contains a non-finite value.");
        if (std::abs(actor.transform.scale[axis]) < 1e-4f) throw std::runtime_error("Actor scale cannot be zero or smaller than 0.0001.");
    }
}
const char* primitiveName(assets::MeshPrimitive primitive) {
    switch (primitive) {
    case assets::MeshPrimitive::Sphere:return "Sphere";
    case assets::MeshPrimitive::Plane:return "Plane";
    case assets::MeshPrimitive::Cube:return "Cube";
    case assets::MeshPrimitive::Imported:return "Imported";
    }
    throw std::runtime_error("Unknown mesh primitive.");
}
assets::AssetId builtinId(const std::string& primitive) {
    if (primitive=="Sphere") return 1;
    if (primitive=="Plane") return 2;
    if (primitive=="Cube") return 3;
    return 0;
}
void validateMesh(const assets::MeshAsset& mesh) {
    readId(Json(mesh.id));
    if (mesh.name.empty() || mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size()%3!=0)
        throw std::runtime_error("Mesh must have a name and indexed triangle geometry.");
    for (auto index:mesh.indices)
        if (index>=mesh.vertices.size()) throw std::runtime_error("Mesh index is outside the vertex buffer.");
    for (const auto& vertex:mesh.vertices) {
        double normalLength=0;
        for (int axis=0;axis<3;++axis) {
            if (!std::isfinite(vertex.position[axis]) || !std::isfinite(vertex.normal[axis]))
                throw std::runtime_error("Mesh contains non-finite geometry.");
            normalLength+=double(vertex.normal[axis])*vertex.normal[axis];
        }
        if (normalLength<1e-12) throw std::runtime_error("Mesh contains zero normals.");
    }
    if (assets::isEngineAssetId(mesh.id) != (mesh.origin == assets::AssetOrigin::Engine))
        throw std::runtime_error("Mesh identity does not match its Engine/Project origin.");
    // Geometry is self-contained. Source paths are provenance, not runtime dependencies.
    if (mesh.origin == assets::AssetOrigin::Engine) return;
    if (mesh.primitive==assets::MeshPrimitive::Imported) {
        if (mesh.id<=3) throw std::runtime_error("Imported mesh IDs 1 to 3 are reserved.");
    } else if (mesh.id!=builtinId(primitiveName(mesh.primitive))) {
        throw std::runtime_error("Built-in meshes must retain their reserved IDs (Sphere 1, Plane 2, Cube 3).");
    }
}
} // namespace

ActorId World::createActor(std::string name, assets::AssetId mesh, assets::AssetId material) {
    Actor actor; actor.id=assets::detail::nextId(actors,nextActorId_);
    actor.name=name.empty()?"Actor":std::move(name); actor.mesh={mesh,material};
    actors.push_back(std::move(actor));
    return actors.back().id;
}
bool World::removeActor(ActorId id) {
    const auto it=std::find_if(actors.begin(),actors.end(),[id](const auto& actor){return actor.id==id;});
    if (it==actors.end()) return false;
    actors.erase(it); return true;
}
ActorId World::duplicateActor(ActorId id) {
    const auto* original=findActor(id);
    if (!original) return 0;
    Actor copy=*original; copy.id=assets::detail::nextId(actors,nextActorId_); copy.name+=" Copy";
    actors.push_back(std::move(copy)); return actors.back().id;
}
Actor* World::findActor(ActorId id) {
    const auto it=std::find_if(actors.begin(),actors.end(),[id](const auto& actor){return actor.id==id;});
    return it==actors.end()?nullptr:&*it;
}
const Actor* World::findActor(ActorId id) const {
    const auto it=std::find_if(actors.begin(),actors.end(),[id](const auto& actor){return actor.id==id;});
    return it==actors.end()?nullptr:&*it;
}
void World::createDefaultScene() {
    environment = {}; startupCamera.reset();
    actors.clear(); nextActorId_=1;
    const auto sphere=createActor("Sphere",1,1);
    findActor(sphere)->transform.position[1]=1;
    createActor("Ground",2,2);
}
void World::createDefaultScene(const assets::AssetLibrary& library) {
    environment = {}; startupCamera.reset();
    const auto pick = [&library](std::string_view name, assets::AssetId fallback) {
        for (const auto& material : library.materials)
            if (material.name == name) return material.id;
        return library.materials.empty() ? fallback : library.materials.front().id;
    };
    actors.clear(); nextActorId_=1;
    const auto sphere=createActor("Sphere",library.findMesh(assets::EngineSphereMeshId) ? assets::EngineSphereMeshId : 1,pick("Copper",1));
    findActor(sphere)->transform.position[1]=1;
    createActor("Ground",library.findMesh(assets::EnginePlaneMeshId) ? assets::EnginePlaneMeshId : 2,pick("Ground",2));
}
bool World::save(const std::filesystem::path& path, const assets::AssetLibrary& library, std::string& error) const {
    try {
        validatePresentation(*this);
        // Version 3 stores asset references only: materials and meshes live in
        // .rasset containers under Content, the scene keeps Actor data.
        Json value={{"format","RAEngine.Scene"},{"version",3},{"actors",Json::array()},
            {"materials",Json::array()},{"meshes",Json::array()}};
        // Written only when non-default so existing sky scenes keep their file shape.
        if (blackBackground) value["background"]="black";
        if (startupCamera) {
            const auto& camera = *startupCamera;
            value["camera"] = {{"yaw",camera.yaw},{"pitch",camera.pitch},{"distance",camera.distance},
                {"target",camera.target},{"exposure",camera.exposure}};
        }
        if (environment.overrideLighting) {
            const auto& light = environment.areaLight;
            value["lighting"] = {{"directionalIntensity",environment.directionalIntensity},
                {"environmentIntensity",environment.environmentIntensity},
                {"areaLight",{{"enabled",light.enabled},{"center",light.center},{"halfAxisU",light.halfAxisU},
                    {"halfAxisV",light.halfAxisV},{"radiance",light.radiance}}}};
        }
        std::unordered_set<assets::AssetId> meshIds,materialIds,actorIds;
        for (const auto& material:library.materials) {
            if (!materialIds.insert(material.id).second) throw std::runtime_error("Duplicate material ID.");
            if (assets::isEngineAssetId(material.id) != (material.origin == assets::AssetOrigin::Engine))
                throw std::runtime_error("Material identity does not match its Engine/Project origin.");
            if (material.origin == assets::AssetOrigin::Engine &&
                std::none_of(actors.begin(), actors.end(), [&](const auto& actor) { return actor.mesh.materialId == material.id; }))
                continue;
            value["materials"].push_back({{"id",material.id}});
        }
        for (const auto& mesh:library.meshes) {
            validateMesh(mesh);
            if (!meshIds.insert(mesh.id).second) throw std::runtime_error("Duplicate mesh ID.");
            if ((mesh.origin == assets::AssetOrigin::Engine || mesh.id <= 3) &&
                std::none_of(actors.begin(), actors.end(), [&](const auto& actor) { return actor.mesh.meshId == mesh.id; }))
                continue;
            value["meshes"].push_back({{"id",mesh.id}});
        }
        for (const auto& actor:actors) {
            validateActor(actor,library);
            if (!actorIds.insert(actor.id).second) throw std::runtime_error("Duplicate actor ID.");
            value["actors"].push_back({{"id",actor.id},{"name",actor.name},{"visible",actor.visible},
                {"transform",{{"position",actor.transform.position},{"rotation",actor.transform.rotation},{"scale",actor.transform.scale}}},
                {"mesh",{{"meshId",actor.mesh.meshId},{"materialId",actor.mesh.materialId}}}});
        }
        assets::detail::writeJson(path,value); error.clear();return true;
    } catch (const std::exception& e) {error=e.what();return false;}
}
bool World::load(const std::filesystem::path& path, assets::AssetLibrary& library, std::string& error) {
    try {
        const auto value=assets::detail::readJson(path);
        const auto version=value.at("version").get<int>();
        if (value.at("format")!="RAEngine.Scene" || (version!=1 && version!=2 && version!=3))
            throw std::runtime_error("Unsupported scene format or version.");
        if (!value.at("actors").is_array() || !value.at("materials").is_array() || !value.at("meshes").is_array())
            throw std::runtime_error("Scene actors, materials, and meshes must be arrays.");
        assets::AssetLibrary builtin; builtin.createDefaults();
        assets::AssetLibrary staged;
        World next;
        readPresentation(value, next);
        if (value.contains("background")) {
            if (!value.at("background").is_string()) throw std::runtime_error("Scene background must be a string.");
            const auto background=value.at("background").get<std::string>();
            if (background=="black") next.blackBackground=true;
            else if (background!="sky") throw std::runtime_error("Unsupported scene background: "+background);
        }
        std::unordered_set<assets::AssetId> meshIds,materialIds,actorIds;
        if (version==1) {
            // Legacy snapshots replace project materials only. Trusted engine
            // content survives and cannot be impersonated by an embedded payload.
            for (const auto& material : library.materials) {
                if (material.origin != assets::AssetOrigin::Engine) continue;
                if (!assets::isEngineAssetId(material.id)) throw std::runtime_error("Invalid Engine material identity.");
                if (!materialIds.insert(material.id).second) throw std::runtime_error("Duplicate Engine material ID.");
                staged.materials.push_back(material);
            }
            for (const auto& item:value.at("materials")) {
                auto material=assets::detail::readMaterial(item);
                if (assets::isEngineAssetId(material.id)) throw std::runtime_error("Legacy scene cannot redefine a reserved Engine material.");
                if (!materialIds.insert(material.id).second) throw std::runtime_error("Duplicate material ID.");
                staged.materials.push_back(std::move(material));
            }
        } else {
            // Version 2+ references materials that were loaded from Content before
            // the scene: every reference must resolve to an existing .rasset.
            staged.materials=library.materials;
            for (const auto& item:value.at("materials")) {
                const auto id=readId(item.at("id"));
                if (!materialIds.insert(id).second) throw std::runtime_error("Duplicate material reference.");
                if (!library.findMaterial(id))
                    throw std::runtime_error("Scene references material " + std::to_string(id) +
                        " that is not loaded from Project or Engine Content/Materials.");
            }
        }
        if (version==3) {
            // Meshes are asset references too: Project Content supplies the geometry.
            staged.meshes=library.meshes;
            for (const auto& item:value.at("meshes")) {
                const auto id=readId(item.at("id"));
                if (!meshIds.insert(id).second) throw std::runtime_error("Duplicate mesh reference.");
                if (!library.findMesh(id))
                    throw std::runtime_error("Scene references mesh " + std::to_string(id) +
                        " that is not loaded from Project or Engine Content.");
            }
        } else {
            const auto sceneFolder=std::filesystem::absolute(path).parent_path();
            for (const auto& mesh : library.meshes) {
                if (mesh.origin != assets::AssetOrigin::Engine) continue;
                validateMesh(mesh);
                if (!meshIds.insert(mesh.id).second) throw std::runtime_error("Duplicate Engine mesh ID.");
                staged.meshes.push_back(mesh);
            }
            for (const auto& item:value.at("meshes")) {
                const auto id=readId(item.at("id"));
                if (assets::isEngineAssetId(id)) throw std::runtime_error("Legacy scene cannot redefine a reserved Engine mesh.");
                if (!meshIds.insert(id).second) throw std::runtime_error("Duplicate mesh ID.");
                const auto primitive=item.at("primitive").get<std::string>();
                assets::MeshAsset mesh;
                if (primitive=="Imported") {
                    if (id<=3) throw std::runtime_error("Imported mesh IDs 1 to 3 are reserved.");
                    const auto source=item.at("sourcePath").get<std::string>();
                    if (source.empty()) throw std::runtime_error("Imported mesh source path is empty.");
                    // Import into an isolated library: file errors never partially alter the active world.
                    assets::AssetLibrary imported;
                    assets::AssetId importedId=0; std::string importError;
                    if (!imported.importObj(sceneFolder/assets::detail::pathFromUtf8(source),importedId,importError))
                        throw std::runtime_error(importError);
                    mesh=std::move(imported.meshes.front());
                } else {
                    const auto reserved=builtinId(primitive);
                    if (reserved==0 || id!=reserved) throw std::runtime_error("Unknown primitive or invalid built-in mesh ID.");
                    mesh=*builtin.findMesh(reserved);
                }
                mesh.id=id; mesh.name=item.at("name").get<std::string>();
                validateMesh(mesh); staged.meshes.push_back(std::move(mesh));
            }
        }
        for (const auto& item:value.at("actors")) {
            Actor actor; actor.id=readId(item.at("id"));
            if (!actorIds.insert(actor.id).second) throw std::runtime_error("Duplicate actor ID.");
            actor.name=item.at("name").get<std::string>(); actor.visible=item.at("visible").get<bool>();
            const auto& transform=item.at("transform");
            assets::detail::readVector(transform.at("position"),actor.transform.position);
            assets::detail::readVector(transform.at("rotation"),actor.transform.rotation);
            assets::detail::readVector(transform.at("scale"),actor.transform.scale);
            actor.mesh.meshId=readId(item.at("mesh").at("meshId"));
            actor.mesh.materialId=readId(item.at("mesh").at("materialId"));
            validateActor(actor,staged); next.actors.push_back(std::move(actor));
        }
        // All parsing, external I/O and reference validation are complete before committing.
        *this=std::move(next);
        if (version==1) library=std::move(staged);
        else if (version==2) library.meshes=std::move(staged.meshes);
        error.clear();return true;
    } catch (const std::exception& e) {error=e.what();return false;}
}
} // namespace rae::scene
