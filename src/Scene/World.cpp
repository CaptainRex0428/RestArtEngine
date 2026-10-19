#include "RAEngine/Scene/World.h"
#include "../Assets/AssetSerialization.h"
#include <algorithm>

namespace rae::scene {
namespace {
using assets::detail::Json;
using assets::detail::readId;
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
    if (mesh.primitive==assets::MeshPrimitive::Imported) {
        if (mesh.id<=3 || mesh.sourcePath.empty() || !std::filesystem::is_regular_file(assets::detail::pathFromUtf8(mesh.sourcePath)))
            throw std::runtime_error("Imported mesh must have an ID above 3 and an existing OBJ source: " + mesh.sourcePath);
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
    actors.clear(); nextActorId_=1;
    const auto sphere=createActor("Sphere",1,1);
    findActor(sphere)->transform.position[1]=1;
    createActor("Ground",2,2);
}
bool World::save(const std::filesystem::path& path, const assets::AssetLibrary& library, std::string& error) const {
    try {
        Json value={{"format","RAEngine.Scene"},{"version",1},{"actors",Json::array()},
            {"materials",Json::array()},{"meshes",Json::array()}};
        std::unordered_set<assets::AssetId> meshIds,materialIds,actorIds;
        for (const auto& material:library.materials) {
            if (!materialIds.insert(material.id).second) throw std::runtime_error("Duplicate material ID.");
            value["materials"].push_back(assets::detail::materialJson(material));
        }
        const auto sceneFolder=std::filesystem::absolute(path).parent_path();
        for (const auto& mesh:library.meshes) {
            validateMesh(mesh);
            if (!meshIds.insert(mesh.id).second) throw std::runtime_error("Duplicate mesh ID.");
            Json data={{"id",mesh.id},{"name",mesh.name},{"primitive",primitiveName(mesh.primitive)}};
            if (mesh.primitive==assets::MeshPrimitive::Imported) {
                const auto source=std::filesystem::absolute(assets::detail::pathFromUtf8(mesh.sourcePath)).lexically_normal();
                const auto relative=source.lexically_relative(sceneFolder);
                data["sourcePath"]=assets::detail::pathUtf8(relative.empty()?source:relative);
            }
            value["meshes"].push_back(std::move(data));
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
        if (value.at("format")!="RAEngine.Scene" || value.at("version")!=1) throw std::runtime_error("Unsupported scene format or version.");
        if (!value.at("actors").is_array() || !value.at("materials").is_array() || !value.at("meshes").is_array())
            throw std::runtime_error("Scene actors, materials, and meshes must be arrays.");
        assets::AssetLibrary builtin; builtin.createDefaults();
        assets::AssetLibrary staged;
        World next;
        std::unordered_set<assets::AssetId> meshIds,materialIds,actorIds;
        for (const auto& item:value.at("materials")) {
            auto material=assets::detail::readMaterial(item);
            if (!materialIds.insert(material.id).second) throw std::runtime_error("Duplicate material ID.");
            staged.materials.push_back(std::move(material));
        }
        const auto sceneFolder=std::filesystem::absolute(path).parent_path();
        for (const auto& item:value.at("meshes")) {
            const auto id=readId(item.at("id"));
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
        *this=std::move(next); library=std::move(staged); error.clear();return true;
    } catch (const std::exception& e) {error=e.what();return false;}
}
} // namespace rae::scene
