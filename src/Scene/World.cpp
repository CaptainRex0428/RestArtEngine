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
        bounded(camera.pitch, -1.5707964f, 1.5707964f, "camera pitch");
        bounded(camera.distance, .001f, 1000000, "camera distance");
        bounded(camera.exposure, .01f, 64, "camera exposure");
        for (float value : camera.target) bounded(value, -1000000, 1000000, "camera target");
        if (!rae::camera::valid(camera.lens)) throw std::runtime_error("Invalid opening camera lens.");
        if (camera.pose && !rae::camera::valid(rae::camera::Camera{*camera.pose, camera.lens}))
            throw std::runtime_error("Invalid opening camera pose.");
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
void validateDefaultCamera(const World& world) {
    if (!world.defaultCameraActor) return;
    readId(Json(world.defaultCameraActor));
    const auto* actor = world.findActor(world.defaultCameraActor);
    if (!actor || !actor->camera)
        throw std::runtime_error("Default camera must reference an existing Camera Actor.");
}
Json writeLens(const camera::Lens& lens) {
    if (!camera::valid(lens)) throw std::runtime_error("Invalid camera lens.");
    Json result = {{"projection", lens.projection == camera::Projection::Perspective ? "perspective" : "orthographic"},
        {"verticalFovDegrees", lens.verticalFovDegrees}, {"orthographicHeight", lens.orthographicHeight},
        {"nearClip", lens.nearClip}, {"farClip", lens.farClip}, {"aspectRatio", lens.aspectRatio}};
    if (lens.depthOfField != camera::DepthOfField{}) {
        const auto& dof = lens.depthOfField;
        result["depthOfField"] = {{"enabled", dof.enabled}, {"focusDistance", dof.focusDistance},
            {"fStop", dof.fStop}, {"sensorHeightMm", dof.sensorHeightMm}};
    }
    return result;
}
camera::Lens readLens(const Json& source, int version) {
    if (!source.is_object()) throw std::runtime_error("Actor camera must be an object.");
    camera::Lens lens;
    const auto projection = source.at("projection").get<std::string>();
    if (projection == "perspective") lens.projection = camera::Projection::Perspective;
    else if (projection == "orthographic") lens.projection = camera::Projection::Orthographic;
    else throw std::runtime_error("Unknown camera projection.");
    lens.verticalFovDegrees = readNumber(source.at("verticalFovDegrees"), "camera field of view");
    lens.orthographicHeight = readNumber(source.at("orthographicHeight"), "camera orthographic height");
    lens.nearClip = readNumber(source.at("nearClip"), "camera near clip");
    lens.farClip = readNumber(source.at("farClip"), "camera far clip");
    lens.aspectRatio = readNumber(source.at("aspectRatio"), "camera aspect ratio");
    if (source.contains("depthOfField")) {
        if (version < 7) throw std::runtime_error("Depth of field requires scene version 7.");
        const auto& input = source.at("depthOfField");
        if (!input.is_object()) throw std::runtime_error("Camera depth of field must be an object.");
        if (!input.at("enabled").is_boolean()) throw std::runtime_error("Camera depth of field enabled must be boolean.");
        auto& dof = lens.depthOfField;
        dof.enabled = input.at("enabled").get<bool>();
        dof.focusDistance = readNumber(input.at("focusDistance"), "camera focus distance");
        dof.fStop = readNumber(input.at("fStop"), "camera f-stop");
        dof.sensorHeightMm = readNumber(input.at("sensorHeightMm"), "camera sensor height");
    }
    if (!camera::valid(lens)) throw std::runtime_error("Invalid camera lens.");
    return lens;
}
bool extendedOpeningCamera(const SceneCamera& camera) {
    const rae::camera::Lens defaults;
    const auto& lens = camera.lens;
    return camera.pose.has_value() || camera.distance < 2 || camera.distance > 30 || std::abs(camera.pitch) > 1.55f ||
        lens.projection != defaults.projection || lens.verticalFovDegrees != defaults.verticalFovDegrees ||
        lens.orthographicHeight != defaults.orthographicHeight || lens.nearClip != defaults.nearClip ||
        lens.farClip != defaults.farClip || lens.aspectRatio != defaults.aspectRatio ||
        lens.depthOfField != defaults.depthOfField;
}
void readPresentation(const Json& value, World& world, int version) {
    if (value.contains("camera")) {
        const auto& input = value.at("camera");
        if (!input.is_object()) throw std::runtime_error("Scene camera must be an object.");
        SceneCamera camera;
        camera.yaw = readNumber(input.at("yaw"), "camera yaw");
        camera.pitch = readNumber(input.at("pitch"), "camera pitch");
        camera.distance = readNumber(input.at("distance"), "camera distance");
        readVector(input.at("target"), camera.target, "camera target");
        camera.exposure = readNumber(input.at("exposure"), "camera exposure");
        if (input.contains("lens") || input.contains("pose")) {
            if (version < 6) throw std::runtime_error("Opening camera lens and pose require scene version 6.");
            if (input.contains("lens")) camera.lens = readLens(input.at("lens"), version);
            if (input.contains("pose")) {
                const auto& source = input.at("pose");
                if (!source.is_object()) throw std::runtime_error("Opening camera pose must be an object.");
                auto& pose = camera.pose.emplace();
                readVector(source.at("position"), pose.position, "opening camera position");
                readVector(source.at("rotation"), pose.rotation, "opening camera rotation");
            }
        }
        if (version < 6) {
            bounded(camera.distance, 2, 30, "legacy camera distance");
            bounded(camera.pitch, -1.55f, 1.55f, "legacy camera pitch");
        }
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
    if (actor.camera) {
        if (actor.light || actor.mesh.meshId || actor.mesh.materialId || !actor.mesh.materialOverrides.empty())
            throw std::runtime_error("A Camera Actor cannot also contain light, mesh or material components.");
        if (!camera::valid(*actor.camera)) throw std::runtime_error("Invalid camera lens.");
        camera::Camera authored;
        authored.lens = *actor.camera;
        std::copy_n(actor.transform.position, 3, authored.pose.position);
        std::copy_n(actor.transform.rotation, 3, authored.pose.rotation);
        if (!camera::valid(authored)) throw std::runtime_error("Camera Actor pose exceeds the supported numeric range.");
    } else if (actor.light) {
        const auto& light = *actor.light;
        if (!parseLightType(lightTypeName(light.type))) throw std::runtime_error("Unknown light type.");
        if (actor.mesh.meshId || actor.mesh.materialId || !actor.mesh.materialOverrides.empty()) throw std::runtime_error("A light Actor cannot also reference a mesh or material.");
        for (float channel : light.color) bounded(channel, 0, 1000000, "light color");
        bounded(light.intensity, 0, 1000000, "light intensity");
        bounded(light.range, .0001f, 1000000, "light range");
        bounded(light.innerConeAngle, 0, light.outerConeAngle, "light inner cone angle");
        bounded(light.outerConeAngle, 0, 89.99f, "light outer cone angle");
        bounded(light.width, .0001f, 1000000, "light width");
        bounded(light.height, .0001f, 1000000, "light height");
    } else {
        if (!library.findMesh(actor.mesh.meshId)) throw std::runtime_error("Actor '"+actor.name+"' references a missing mesh.");
        if (!library.findMaterial(actor.mesh.materialId)) throw std::runtime_error("Actor '"+actor.name+"' references a missing material.");
        const auto& mesh = *library.findMesh(actor.mesh.meshId);
        if (actor.mesh.materialOverrides.size() > mesh.materialSlotCount())
            throw std::runtime_error("Actor material overrides exceed the mesh's slot count.");
        std::unordered_set<std::string> slotKeys;
        for (const auto& slot : mesh.materialSlots) {
            const auto& key = slot.sourceKey.empty() ? slot.name : slot.sourceKey;
            if (key.empty() || !slotKeys.insert(key).second)
                throw std::runtime_error("Mesh material slots require unique nonempty source keys or fallback names.");
        }
        for (const auto id : actor.mesh.materialOverrides)
            if (id && !library.findMaterial(id)) throw std::runtime_error("Actor references a missing slot material.");
        for (const auto& slot : mesh.materialSlots)
            if (slot.defaultMaterialId && !library.findMaterial(slot.defaultMaterialId))
                throw std::runtime_error("Mesh slot references a missing default material.");
    }
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
ActorId World::createLight(std::string name, LightType type) {
    if (!parseLightType(lightTypeName(type))) return 0;
    Actor actor; actor.id = assets::detail::nextId(actors, nextActorId_);
    actor.name = name.empty() ? std::string(lightTypeName(type)) + " Light" : std::move(name);
    actor.mesh = {0, 0};
    actor.light.emplace(); actor.light->type = type;
    actor.transform.position[1] = 3;
    if (type == LightType::Sky) {
        actor.light->intensity = 1;
        actor.light->castShadows = false;
    } else if (type == LightType::Directional) {
        actor.light->intensity = 5;
        actor.transform.rotation[0] = -45;
        actor.transform.rotation[1] = -30;
    } else if (type == LightType::Rect || type == LightType::Spot) {
        actor.transform.rotation[0] = -90;
    }
    actors.push_back(std::move(actor));
    return actors.back().id;
}
ActorId World::createCamera(std::string name) {
    Actor actor;
    actor.id = assets::detail::nextId(actors, nextActorId_);
    actor.name = name.empty() ? "Camera" : std::move(name);
    actor.mesh = {0, 0};
    actor.camera.emplace();
    actors.push_back(std::move(actor));
    return actors.back().id;
}
bool World::removeActor(ActorId id) {
    const auto it=std::find_if(actors.begin(),actors.end(),[id](const auto& actor){return actor.id==id;});
    if (it==actors.end()) return false;
    actors.erase(it);
    if (defaultCameraActor == id) defaultCameraActor = 0;
    return true;
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
    defaultCameraActor = 0;
    actors.clear(); nextActorId_=1;
    const auto sphere=createActor("Sphere",1,1);
    findActor(sphere)->transform.position[1]=1;
    createActor("Ground",2,2);
}
void World::createDefaultScene(const assets::AssetLibrary& library) {
    environment = {}; startupCamera.reset();
    defaultCameraActor = 0;
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
        validateDefaultCamera(*this);
        // Versions 4/5 add lights/material slots; version 6 adds Camera Actors
        // and the explicit default output camera; version 7 adds authored DOF.
        // Default/disabled-unmodified DOF keeps older scenes byte-shape compatible.
        const bool hasLights = std::any_of(actors.begin(), actors.end(), [](const Actor& actor) { return actor.light.has_value(); });
        const bool hasCameras = std::any_of(actors.begin(), actors.end(), [](const Actor& actor) { return actor.camera.has_value(); });
        const bool hasExtendedOpeningCamera = startupCamera && extendedOpeningCamera(*startupCamera);
        const bool hasDepthOfField = (startupCamera && startupCamera->lens.depthOfField != camera::DepthOfField{}) ||
            std::any_of(actors.begin(), actors.end(), [](const Actor& actor) {
                return actor.camera && actor.camera->depthOfField != camera::DepthOfField{};
            });
        const bool hasMaterialOverrides = std::any_of(actors.begin(), actors.end(),
            [](const Actor& actor) { return !actor.mesh.materialOverrides.empty(); });
        Json value={{"format","RAEngine.Scene"},{"version",hasDepthOfField ? 7 : hasCameras || hasExtendedOpeningCamera ? 6 : hasMaterialOverrides ? 5 : hasLights ? 4 : 3},{"actors",Json::array()},
            {"materials",Json::array()},{"meshes",Json::array()}};
        if (defaultCameraActor) value["defaultCameraActor"] = defaultCameraActor;
        // Written only when non-default so existing sky scenes keep their file shape.
        if (blackBackground) value["background"]="black";
        if (startupCamera) {
            const auto& camera = *startupCamera;
            value["camera"] = {{"yaw",camera.yaw},{"pitch",camera.pitch},{"distance",camera.distance},
                {"target",camera.target},{"exposure",camera.exposure}};
            if (hasExtendedOpeningCamera) value["camera"]["lens"] = writeLens(camera.lens);
            if (camera.pose) value["camera"]["pose"] = {{"position", camera.pose->position}, {"rotation", camera.pose->rotation}};
        }
        if (environment.overrideLighting) {
            const auto& light = environment.areaLight;
            value["lighting"] = {{"directionalIntensity",environment.directionalIntensity},
                {"environmentIntensity",environment.environmentIntensity},
                {"areaLight",{{"enabled",light.enabled},{"center",light.center},{"halfAxisU",light.halfAxisU},
                    {"halfAxisV",light.halfAxisV},{"radiance",light.radiance}}}};
        }
        std::unordered_set<assets::AssetId> meshIds,materialIds,actorIds;
        std::unordered_set<assets::AssetId> referencedMeshes, referencedMaterials;
        for (const auto& actor : actors) {
            if (actor.light || actor.camera) continue;
            referencedMeshes.insert(actor.mesh.meshId);
            referencedMaterials.insert(actor.mesh.materialId);
            for (const auto id : actor.mesh.materialOverrides) if (id) referencedMaterials.insert(id);
            if (const auto* mesh = library.findMesh(actor.mesh.meshId))
                for (const auto& slot : mesh->materialSlots)
                    if (slot.defaultMaterialId) referencedMaterials.insert(slot.defaultMaterialId);
        }
        for (const auto& material:library.materials) {
            readId(Json(material.id));
            if (!materialIds.insert(material.id).second) throw std::runtime_error("Duplicate material ID.");
            if (assets::isEngineAssetId(material.id) != (material.origin == assets::AssetOrigin::Engine))
                throw std::runtime_error("Material identity does not match its Engine/Project origin.");
            if (!referencedMaterials.contains(material.id)) continue;
            value["materials"].push_back({{"id",material.id}});
        }
        for (const auto& mesh:library.meshes) {
            readId(Json(mesh.id));
            if (!meshIds.insert(mesh.id).second) throw std::runtime_error("Duplicate mesh ID.");
            if (assets::isEngineAssetId(mesh.id) != (mesh.origin == assets::AssetOrigin::Engine))
                throw std::runtime_error("Mesh identity does not match its Engine/Project origin.");
            if (!referencedMeshes.contains(mesh.id)) continue;
            value["meshes"].push_back({{"id",mesh.id}});
        }
        for (const auto& actor:actors) {
            validateActor(actor,library);
            if (!actorIds.insert(actor.id).second) throw std::runtime_error("Duplicate actor ID.");
            Json item = {{"id",actor.id},{"name",actor.name},{"visible",actor.visible},
                {"transform",{{"position",actor.transform.position},{"rotation",actor.transform.rotation},{"scale",actor.transform.scale}}}};
            if (actor.camera) {
                item["camera"] = writeLens(*actor.camera);
            } else if (actor.light) {
                const auto& light = *actor.light;
                item["light"] = {{"type",lightTypeName(light.type)},{"color",light.color},
                    {"intensity",light.intensity},{"range",light.range},{"innerConeAngle",light.innerConeAngle},
                    {"outerConeAngle",light.outerConeAngle},{"width",light.width},{"height",light.height},
                    {"castShadows",light.castShadows}};
            } else {
                item["mesh"] = {{"meshId",actor.mesh.meshId},{"materialId",actor.mesh.materialId}};
                if (!actor.mesh.materialOverrides.empty()) {
                    item["mesh"]["materialOverrides"] = actor.mesh.materialOverrides;
                    auto& keys = item["mesh"]["materialOverrideKeys"] = Json::array();
                    const auto& mesh = *library.findMesh(actor.mesh.meshId);
                    for (std::size_t slot = 0; slot < actor.mesh.materialOverrides.size(); ++slot) {
                        const auto key = slot < mesh.materialSlots.size()
                            ? (mesh.materialSlots[slot].sourceKey.empty() ? mesh.materialSlots[slot].name : mesh.materialSlots[slot].sourceKey)
                            : std::string{};
                        keys.push_back(key);
                    }
                }
            }
            value["actors"].push_back(std::move(item));
        }
        assets::detail::writeJson(path,value); error.clear();return true;
    } catch (const std::exception& e) {error=e.what();return false;}
}
bool World::load(const std::filesystem::path& path, assets::AssetLibrary& library, std::string& error) {
    try {
        const auto value=assets::detail::readJson(path);
        const auto version=value.at("version").get<int>();
        if (value.at("format")!="RAEngine.Scene" || (version < 1 || version > 7))
            throw std::runtime_error("Unsupported scene format or version.");
        if (!value.at("actors").is_array() || !value.at("materials").is_array() || !value.at("meshes").is_array())
            throw std::runtime_error("Scene actors, materials, and meshes must be arrays.");
        assets::AssetLibrary builtin; builtin.createDefaults();
        assets::AssetLibrary staged;
        World next;
        readPresentation(value, next, version);
        if (value.contains("defaultCameraActor")) {
            if (version < 6) throw std::runtime_error("Default camera requires scene version 6.");
            const auto& id = value.at("defaultCameraActor");
            if (!id.is_number_integer()) throw std::runtime_error("Default camera must be an integer Actor ID or zero.");
            next.defaultCameraActor = id == 0 ? 0 : readId(id);
        }
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
        if (version>=3) {
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
            if (item.contains("camera")) {
                if (version < 6) throw std::runtime_error("Camera Actors require scene version 6.");
                if (item.contains("light") || item.contains("mesh"))
                    throw std::runtime_error("An Actor cannot contain camera and light or mesh components.");
                actor.mesh = {0, 0};
                actor.camera = readLens(item.at("camera"), version);
            } else if (item.contains("light")) {
                if (version < 4) throw std::runtime_error("Light Actors require scene version 4.");
                if (item.contains("mesh")) throw std::runtime_error("An Actor cannot contain both light and mesh components.");
                const auto& source = item.at("light");
                if (!source.is_object()) throw std::runtime_error("Actor light must be an object.");
                const auto type = parseLightType(source.at("type").get<std::string>());
                if (!type) throw std::runtime_error("Unknown light type.");
                actor.mesh = {0, 0};
                auto& light = actor.light.emplace(); light.type = *type;
                readVector(source.at("color"), light.color, "light color");
                light.intensity = readNumber(source.at("intensity"), "light intensity");
                light.range = readNumber(source.at("range"), "light range");
                light.innerConeAngle = readNumber(source.at("innerConeAngle"), "light inner cone angle");
                light.outerConeAngle = readNumber(source.at("outerConeAngle"), "light outer cone angle");
                light.width = readNumber(source.at("width"), "light width");
                light.height = readNumber(source.at("height"), "light height");
                if (!source.at("castShadows").is_boolean()) throw std::runtime_error("Light castShadows must be boolean.");
                light.castShadows = source.at("castShadows").get<bool>();
            } else {
                actor.mesh.meshId=readId(item.at("mesh").at("meshId"));
                actor.mesh.materialId=readId(item.at("mesh").at("materialId"));
                if (item.at("mesh").contains("materialOverrides")) {
                    const auto& overrides = item.at("mesh").at("materialOverrides");
                    const auto* mesh = staged.findMesh(actor.mesh.meshId);
                    if (version < 5 || !overrides.is_array() || !mesh || overrides.size() > 65536)
                        throw std::runtime_error("Invalid material slot overrides; requires scene version 5 and valid slot indices.");
                    std::vector<assets::AssetId> parsed;
                    for (const auto& id : overrides) {
                        if (!id.is_number_integer()) throw std::runtime_error("Material override must be an integer GUID or zero.");
                        parsed.push_back(id == 0 ? 0 : readId(id));
                    }
                    if (item.at("mesh").contains("materialOverrideKeys")) {
                        const auto& keys = item.at("mesh").at("materialOverrideKeys");
                        if (!keys.is_array() || keys.size() != parsed.size())
                            throw std::runtime_error("Material override keys must match the override array.");
                        actor.mesh.materialOverrides.resize(mesh->materialSlotCount());
                        std::unordered_set<std::string> usedKeys;
                        for (std::size_t slot = 0; slot < parsed.size(); ++slot) {
                            const auto key = keys.at(slot).get<std::string>();
                            if (!usedKeys.insert(key).second) throw std::runtime_error("Duplicate material override key.");
                            if (key.empty() && mesh->materialSlots.empty() && slot == 0) {
                                actor.mesh.materialOverrides[0] = parsed[slot];
                                continue;
                            }
                            std::optional<std::size_t> match;
                            for (std::size_t target = 0; target < mesh->materialSlots.size(); ++target) {
                                const auto& source = mesh->materialSlots[target];
                                if ((source.sourceKey.empty() ? source.name : source.sourceKey) != key) continue;
                                if (match) throw std::runtime_error("Ambiguous mesh material slot key.");
                                match = target;
                            }
                            // Removed source slots no longer bind any surface. New slots inherit.
                            if (match) actor.mesh.materialOverrides[*match] = parsed[slot];
                        }
                    } else {
                        if (parsed.size() > mesh->materialSlotCount()) throw std::runtime_error("Material overrides exceed slot count.");
                        actor.mesh.materialOverrides = std::move(parsed);
                    }
                }
            }
            validateActor(actor,staged); next.actors.push_back(std::move(actor));
        }
        validateDefaultCamera(next);
        // All parsing, external I/O and reference validation are complete before committing.
        *this=std::move(next);
        if (version==1) library=std::move(staged);
        else if (version==2) library.meshes=std::move(staged.meshes);
        error.clear();return true;
    } catch (const std::exception& e) {error=e.what();return false;}
}
} // namespace rae::scene
