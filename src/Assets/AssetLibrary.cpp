#include "RestArtEngine/Assets/AssetLibrary.h"
#include "RestArtEngine/Assets/AssetFile.h"
#include "RestArtEngine/Assets/AssetThumbnail.h"
#include "AssetSerialization.h"
#include "ContentFiles.h"
#include "FbxImporter.h"
#include <unordered_map>
#include <algorithm>
#include <array>
#include <charconv>
#include <sstream>

namespace rae::assets {
namespace {
using Vec3 = std::array<float, 3>;

struct StoredAssetPath {
    std::filesystem::path path;
    int priority = 0; // .rasset > .raasset > legacy JSON
};
using StoredAssetPaths = std::unordered_map<AssetId, StoredAssetPath>;

StoredAssetPaths storedAssetPaths(const std::filesystem::path& folder, AssetFileType type) {
    if (detail::contentLink(folder)) throw std::runtime_error("Asset folder must not be a linked directory.");
    StoredAssetPaths result;
    for (const auto& path : detail::contentFiles(folder)) {
        AssetId id = 0;
        int priority = 0;
        if (isAssetContainerFile(path)) {
            AssetFile file;
            std::string error;
            if (!readAssetFileHeader(path, file, error) || file.type != type) continue;
            id = file.id;
            priority = path.extension() == ".rasset" ? 2 : 1;
        } else if (type == AssetFileType::Material && path.extension() == ".ramat") {
            // A malformed unrelated legacy file is never overwritten by saving.
            try { id = detail::readMaterial(detail::readJson(path).at("material")).id; }
            catch (const std::exception&) { continue; }
        } else continue;
        const auto found = result.find(id);
        if (found != result.end() && found->second.priority == priority && priority > 0)
            throw std::runtime_error("Duplicate asset GUID: " + detail::pathUtf8(path));
        if (found == result.end() || priority > found->second.priority) result[id] = {path, priority};
    }
    return result;
}

std::string pathKey(const std::filesystem::path& path) {
    auto key = detail::pathUtf8(path.lexically_normal());
#ifdef _WIN32
    for (char& value : key) if (value >= 'A' && value <= 'Z') value += 'a' - 'A';
#endif
    return key;
}

std::filesystem::path saveAssetPath(const std::filesystem::path& folder, const std::string& fallback,
    AssetId id, const StoredAssetPaths& existing, std::unordered_set<std::string>& reserved) {
    const auto found = existing.find(id);
    auto path = found == existing.end() ? folder / detail::pathFromUtf8(fallback) : found->second.path;
    path.replace_extension(".rasset");
    const auto base = path;
    unsigned suffix = 0;
    while (reserved.contains(pathKey(path)) || (std::filesystem::exists(path) &&
        !(found != existing.end() && found->second.path == path))) {
        path = base.parent_path() / detail::pathFromUtf8(detail::pathUtf8(base.stem()) + "-" + assetIdText(id) +
            (suffix ? "-" + std::to_string(suffix) : std::string{}) + ".rasset");
        ++suffix;
    }
    if (detail::contentLink(path)) throw std::runtime_error("Cannot save through a linked asset file.");
    reserved.insert(pathKey(path));
    return path;
}

Vec3 normalOf(const Vec3& a, const Vec3& b, const Vec3& c) {
    const double ux = double(b[0]) - a[0], uy = double(b[1]) - a[1], uz = double(b[2]) - a[2];
    const double vx = double(c[0]) - a[0], vy = double(c[1]) - a[1], vz = double(c[2]) - a[2];
    const double nx = uy*vz-uz*vy, ny = uz*vx-ux*vz, nz = ux*vy-uy*vx;
    const double length = std::sqrt(nx*nx+ny*ny+nz*nz);
    if (!std::isfinite(length) || length < 1e-12) throw std::runtime_error("Degenerate OBJ triangle.");
    return {float(nx/length),float(ny/length),float(nz/length)};
}
std::size_t objIndex(const std::string& text, std::size_t count) {
    std::int64_t value = 0;
    const auto parsed = std::from_chars(text.data(),text.data()+text.size(),value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size() || value == 0)
        throw std::runtime_error("Invalid OBJ index: " + text);
    const auto index = value > 0 ? value - 1 : static_cast<std::int64_t>(count) + value;
    if (index < 0 || static_cast<std::uint64_t>(index) >= count) throw std::runtime_error("OBJ index out of range.");
    return static_cast<std::size_t>(index);
}
MeshAsset sphereMesh() {
    MeshAsset mesh; mesh.id = 1; mesh.name = "Sphere"; mesh.primitive = MeshPrimitive::Sphere;
    constexpr std::uint32_t rings = 64, segments = 96;
    constexpr float pi = 3.14159265358979323846f;
    for (std::uint32_t ring=0; ring<=rings; ++ring) {
        const float theta=pi*float(ring)/rings;
        for (std::uint32_t segment=0; segment<=segments; ++segment) {
            const float phi=2*pi*float(segment)/segments;
            const float x=std::sin(theta)*std::cos(phi),y=std::cos(theta),z=std::sin(theta)*std::sin(phi);
            mesh.vertices.push_back({{x,y,z},{x,y,z}});
        }
    }
    for (std::uint32_t ring=0; ring<rings; ++ring)
        for (std::uint32_t segment=0; segment<segments; ++segment) {
            const auto a=ring*(segments+1)+segment,b=a+segments+1;
            // Keep geometric winding aligned with the outward vertex normals.
            if (ring != 0) mesh.indices.insert(mesh.indices.end(),{a,a+1,b});
            if (ring != rings-1) mesh.indices.insert(mesh.indices.end(),{a+1,b+1,b});
        }
    return mesh;
}
MeshAsset planeMesh() {
    MeshAsset mesh; mesh.id=2; mesh.name="Plane"; mesh.primitive=MeshPrimitive::Plane;
    mesh.vertices={{{-12,0,-12},{0,1,0}},{{12,0,-12},{0,1,0}},{{12,0,12},{0,1,0}},{{-12,0,12},{0,1,0}}};
    mesh.indices={0,2,1,0,3,2};
    return mesh;
}
MeshAsset cubeMesh() {
    MeshAsset mesh; mesh.id=3; mesh.name="Cube"; mesh.primitive=MeshPrimitive::Cube;
    const Vec3 normals[]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    const Vec3 corners[][4]={
        {{1,-1,-1},{1,1,-1},{1,1,1},{1,-1,1}},
        {{-1,-1,1},{-1,1,1},{-1,1,-1},{-1,-1,-1}},
        {{-1,1,-1},{-1,1,1},{1,1,1},{1,1,-1}},
        {{-1,-1,1},{-1,-1,-1},{1,-1,-1},{1,-1,1}},
        {{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}},
        {{1,-1,-1},{-1,-1,-1},{-1,1,-1},{1,1,-1}}};
    for (std::uint32_t face=0;face<6;++face) {
        for (const auto& p:corners[face]) mesh.vertices.push_back({{p[0],p[1],p[2]},{normals[face][0],normals[face][1],normals[face][2]}});
        const auto start=face*4;
        mesh.indices.insert(mesh.indices.end(),{start,start+1,start+2,start,start+2,start+3});
    }
    return mesh;
}
} // namespace

void AssetLibrary::createDefaults() {
    persistedMeshes_.clear();
    meshes.clear(); materials.clear();
    meshes.push_back(sphereMesh()); meshes.push_back(planeMesh()); meshes.push_back(cubeMesh());
    MaterialAsset copper; copper.id=1; copper.name="Copper";
    MaterialAsset ground; ground.id=2; ground.name="Ground";
    ground.baseColor[0]=.22f; ground.baseColor[1]=.25f; ground.baseColor[2]=.3f;
    ground.roughness=.82f; ground.metallic=.04f;
    ground.grid=true;
    materials={copper,ground};
}
MeshAsset* AssetLibrary::findMesh(AssetId id) {
    const auto it=std::find_if(meshes.begin(),meshes.end(),[id](const auto& m){return m.id==id;});
    return it==meshes.end()?nullptr:&*it;
}
const MeshAsset* AssetLibrary::findMesh(AssetId id) const {
    const auto it=std::find_if(meshes.begin(),meshes.end(),[id](const auto& m){return m.id==id;});
    return it==meshes.end()?nullptr:&*it;
}
MaterialAsset* AssetLibrary::findMaterial(AssetId id) {
    const auto it=std::find_if(materials.begin(),materials.end(),[id](const auto& m){return m.id==id;});
    return it==materials.end()?nullptr:&*it;
}
const MaterialAsset* AssetLibrary::findMaterial(AssetId id) const {
    const auto it=std::find_if(materials.begin(),materials.end(),[id](const auto& m){return m.id==id;});
    return it==materials.end()?nullptr:&*it;
}
AssetId AssetLibrary::createMaterial(std::string name) {
    if (findMaterial(EngineDefaultMaterialId))
        return cloneMaterial(EngineDefaultMaterialId, name.empty() ? "Material" : std::move(name));
    MaterialAsset material;
    do { material.id=generateAssetId(); } while (findMaterial(material.id));
    material.name=name.empty()?"Material":std::move(name);
    materials.push_back(std::move(material));
    return materials.back().id;
}
AssetId AssetLibrary::cloneMaterial(AssetId sourceId, std::string name) {
    const auto* source = findMaterial(sourceId);
    if (!source) throw std::runtime_error("Cannot copy a material that is not loaded.");
    auto material = *source;
    detail::validateMaterial(material);
    do { material.id=generateAssetId(); } while (findMaterial(material.id));
    material.origin = AssetOrigin::Project;
    material.name = name.empty() ? material.name + " Copy" : std::move(name);
    materials.push_back(std::move(material));
    return materials.back().id;
}
bool AssetLibrary::importObj(const std::filesystem::path& path, AssetId& result, std::string& error) {
    try {
        std::ifstream stream(path);
        if (!stream) throw std::runtime_error("Cannot open OBJ: " + detail::pathUtf8(path));
        MeshAsset mesh; mesh.name=detail::pathUtf8(path.stem()); mesh.primitive=MeshPrimitive::Imported;
        mesh.sourcePath=detail::pathUtf8(std::filesystem::absolute(path).lexically_normal());
        std::vector<Vec3> positions,normals;
        std::vector<std::array<float, 2>> textures;
        std::size_t lineNumber=0;
        std::unordered_map<std::string, std::uint32_t> slots;
        std::vector<std::vector<std::uint32_t>> sectionIndices;
        std::string materialName;
        const auto slotForName = [&](const std::string& name) {
            if (const auto found = slots.find(name); found != slots.end()) return found->second;
            const auto slot = static_cast<std::uint32_t>(mesh.materialSlots.size());
            mesh.materialSlots.push_back({name.empty() ? "Default" : name, "obj:" + name, 0});
            slots.emplace(name, slot); sectionIndices.emplace_back();
            return slot;
        };
        std::string line;
        while (std::getline(stream,line)) {
            ++lineNumber;
            try {
                if (const auto comment=line.find('#');comment!=std::string::npos) line.resize(comment);
                std::istringstream input(line); std::string command; input>>command;
                if (command=="v" || command=="vn") {
                    Vec3 value{};
                    if (!(input>>value[0]>>value[1]>>value[2])) throw std::runtime_error("Invalid vertex or normal.");
                    for (float item:value) if (!std::isfinite(item) || std::abs(item)>1e9f) throw std::runtime_error("Vertex or normal exceeds the finite supported range (+/-1e9).");
                    if (command=="vn") {
                        const double length=std::sqrt(double(value[0])*value[0]+double(value[1])*value[1]+double(value[2])*value[2]);
                        if (length<1e-12) throw std::runtime_error("OBJ normal cannot be zero.");
                        for (float& item:value) item=static_cast<float>(item/length);
                        normals.push_back(value);
                    } else positions.push_back(value);
                } else if (command=="vt") {
                    std::array<float, 2> value{};
                    if (!(input >> value[0]) || !std::isfinite(value[0])) throw std::runtime_error("Invalid OBJ texture coordinate.");
                    input >> std::ws;
                    if (!input.eof() && (!(input >> value[1]) || !std::isfinite(value[1])))
                        throw std::runtime_error("Invalid OBJ texture coordinate.");
                    input >> std::ws;
                    float third = 0;
                    if (!input.eof() && (!(input >> third) || !std::isfinite(third)))
                        throw std::runtime_error("Invalid OBJ texture coordinate.");
                    textures.push_back(value);
                } else if (command=="usemtl") {
                    std::getline(input >> std::ws, materialName);
                    while (!materialName.empty() && std::isspace(static_cast<unsigned char>(materialName.back()))) materialName.pop_back();
                    if (materialName.empty()) throw std::runtime_error("OBJ usemtl requires a material name.");
                    (void)slotForName(materialName);
                } else if (command=="f") {
                    const auto materialSlot = slotForName(materialName);
                    struct Corner {std::size_t position; std::size_t normal=0, texture=0; bool hasNormal=false, hasTexture=false;};
                    std::vector<Corner> face; std::string token;
                    while (input>>token) {
                        const auto slash=token.find('/');
                        Corner corner{objIndex(token.substr(0,slash),positions.size())};
                        if (slash!=std::string::npos) {
                            const auto second=token.find('/',slash+1);
                            const auto texture=token.substr(slash+1,second==std::string::npos?std::string::npos:second-slash-1);
                            if (!texture.empty()) { corner.texture=objIndex(texture,textures.size()); corner.hasTexture=true; }
                            if (second!=std::string::npos) {
                                const auto normal=token.substr(second+1);
                                if (normal.empty()) throw std::runtime_error("Missing normal index after second slash.");
                                corner.normal=objIndex(normal,normals.size()); corner.hasNormal=true;
                            }
                        }
                        face.push_back(corner);
                        if (face.size()>65536) throw std::runtime_error("OBJ polygon is too large.");
                    }
                    if (face.size()<3) throw std::runtime_error("OBJ face needs at least three vertices.");
                    for (std::size_t i=1;i+1<face.size();++i) {
                        const Corner triangle[]={face[0],face[i],face[i+1]};
                        const auto normal=normalOf(positions[triangle[0].position],positions[triangle[1].position],positions[triangle[2].position]);
                        for (const auto& corner:triangle) {
                            const auto& p=positions[corner.position]; const auto& n=corner.hasNormal?normals[corner.normal]:normal;
                            if (corner.hasTexture && mesh.uvNames[0].empty()) {
                                mesh.uvNames[0] = "UV0";
                                mesh.uvChannels[0].resize(mesh.vertices.size());
                            }
                            sectionIndices[materialSlot].push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
                            mesh.vertices.push_back({{p[0],p[1],p[2]},{n[0],n[1],n[2]}});
                            if (!mesh.uvNames[0].empty()) mesh.uvChannels[0].push_back(corner.hasTexture ? textures[corner.texture] : std::array<float,2>{});
                        }
                    }
                }
                if (positions.size()>4000000 || mesh.vertices.size()>12000000) throw std::runtime_error("OBJ exceeds the editor mesh size limit.");
            } catch (const std::exception& e) {throw std::runtime_error("OBJ line " + std::to_string(lineNumber) + ": " + e.what());}
        }
        if (stream.bad()) throw std::runtime_error("Failed reading OBJ.");
        for (std::size_t slot = 0; slot < sectionIndices.size(); ++slot) {
            const auto& indices = sectionIndices[slot];
            if (indices.empty()) continue;
            mesh.sections.push_back({static_cast<std::uint32_t>(mesh.indices.size()),
                static_cast<std::uint32_t>(indices.size()), static_cast<std::uint32_t>(slot)});
            mesh.indices.insert(mesh.indices.end(), indices.begin(), indices.end());
        }
        if (mesh.indices.empty()) throw std::runtime_error("OBJ contains no renderable faces.");
        do { mesh.id = generateAssetId(); } while (findMesh(mesh.id));
        meshes.push_back(std::move(mesh));
        result=meshes.back().id; error.clear(); return true;
    } catch (const std::exception& e) {error=e.what();return false;}
}
bool AssetLibrary::importFbx(const std::filesystem::path& path, AssetId& result, std::string& error,
    MeshImportDiagnostics* diagnostics, const AssetImportOptions& options) {
    if (diagnostics) *diagnostics = {};
    try {
        MeshAsset mesh;
        MeshImportDiagnostics importDiagnostics;
        if (!detail::readFbxMesh(path, mesh, error, &importDiagnostics, options)) return false;
        do { mesh.id = generateAssetId(); } while (findMesh(mesh.id));
        meshes.push_back(std::move(mesh));
        result = meshes.back().id;
        if (diagnostics) *diagnostics = importDiagnostics;
        error.clear();
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

namespace {
bool persistMaterial(const MaterialAsset& material, const std::filesystem::path& path, std::string& error) {
    auto file = makeMaterialAssetFile(material);
    AssetFile stored;
    std::string readError;
    if (path.extension() == ".rasset" && readAssetFileHeader(path, stored, readError) &&
        stored.type == AssetFileType::Material && stored.id == material.id && stored.name == material.name &&
        stored.manifest.contains("payload") && stored.manifest.at("payload") == file.manifest.at("payload")) {
        error.clear();
        return true;
    }
    attachThumbnail(file, renderMaterialThumbnail(material));
    return writeAssetFile(path, file, error);
}

void validateMeshForPersistence(const MeshAsset& mesh) {
    detail::readId(detail::Json(mesh.id));
    if (mesh.name.empty() || mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size() % 3 != 0)
        throw std::runtime_error("Mesh must have a name and indexed triangle geometry.");
    if (mesh.primitive == MeshPrimitive::Imported && mesh.id <= 3)
        throw std::runtime_error("Imported mesh IDs 1 to 3 are reserved.");
    for (const auto index : mesh.indices)
        if (index >= mesh.vertices.size()) throw std::runtime_error("Mesh index is outside the vertex buffer.");
    for (const auto& vertex : mesh.vertices) {
        double normalLength = 0;
        for (unsigned axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(vertex.position[axis]) || !std::isfinite(vertex.normal[axis]))
                throw std::runtime_error("Mesh contains non-finite geometry.");
            normalLength += double(vertex.normal[axis]) * vertex.normal[axis];
        }
        if (normalLength < 1e-12) throw std::runtime_error("Mesh contains zero normals.");
    }
}
} // namespace

bool AssetLibrary::saveMaterial(AssetId id, const std::filesystem::path& folder, std::string& error) const {
    try {
        const auto* material = findMaterial(id);
        if (!material) throw std::runtime_error("Cannot save a material that is not loaded.");
        if (std::count_if(materials.begin(), materials.end(), [id](const auto& item) { return item.id == id; }) != 1)
            throw std::runtime_error("Duplicate material ID.");
        if (material->origin == AssetOrigin::Engine || isEngineAssetId(id))
            throw std::runtime_error("Engine material is read-only. Copy it to the project before saving.");
        detail::validateMaterial(*material);
        const auto existing = storedAssetPaths(folder, AssetFileType::Material);
        std::unordered_set<std::string> reserved;
        const auto path = saveAssetPath(folder, materialAssetFileName(*material), id, existing, reserved);
        return persistMaterial(*material, path, error);
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

bool AssetLibrary::saveMaterials(const std::filesystem::path& folder, std::string& error) const {
    try {
        const auto existing = storedAssetPaths(folder, AssetFileType::Material);
        std::unordered_set<AssetId> ids;
        std::unordered_set<std::string> reserved;
        // Validate ownership before any write, so a forged project identity
        // cannot partially save the project before being rejected.
        for (const auto& material : materials) {
            if (!ids.insert(material.id).second) throw std::runtime_error("Duplicate material ID.");
            if (isEngineAssetId(material.id) != (material.origin == AssetOrigin::Engine))
                throw std::runtime_error("Material identity does not match its Engine/Project origin.");
            detail::validateMaterial(material);
        }
        for (const auto& material : materials) {
            if (material.origin == AssetOrigin::Engine) continue;
            // GUID owns its existing file, irrespective of browser move/rename.
            // New assets use a GUID suffix when their display names collide.
            const auto path = saveAssetPath(folder, materialAssetFileName(material), material.id, existing, reserved);
            if (!persistMaterial(material, path, error)) return false;
        }
        // Legacy sources and unrelated files belong to the project. Saving is
        // not permission to remove them, including old .raasset containers.
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

std::string AssetLibrary::materialAssetFileName(const MaterialAsset& material) {
    return sanitizeAssetName(material.name) + ".rasset";
}

namespace {
std::vector<MaterialAsset> readMaterialFolder(const std::filesystem::path& folder, AssetOrigin origin) {
        std::string error;
        if (!std::filesystem::is_directory(folder) || detail::contentLink(folder))
            throw std::runtime_error("Material directory does not exist or is linked: " + detail::pathUtf8(folder));
        std::vector<MaterialAsset> loaded;
        std::unordered_set<AssetId> ids;
        const auto files = detail::contentFiles(folder);
        // Process modern containers first; same-GUID legacy files are fallbacks.
        for (int pass = 0; pass < 3; ++pass) {
            const auto preferredIds = ids;
            for (const auto& path : files) {
                const auto extension = path.extension();
                if ((pass == 0 && extension != ".rasset") || (pass == 1 && extension != ".raasset") ||
                    (pass == 2 && extension != ".ramat")) continue;
                MaterialAsset material;
                if (pass < 2) {
                    AssetFile file;
                    if (!readAssetFileHeader(path, file, error)) throw std::runtime_error(error);
                    if (file.type != AssetFileType::Material) continue;
                    if (pass == 1 && preferredIds.contains(file.id)) continue;
                    if (!readAssetFile(path, file, error) || !readMaterialAssetFile(file, material, error))
                        throw std::runtime_error(error);
                } else {
                    const auto value = detail::readJson(path);
                    if (value.at("format") != "RAEngine.Material" || value.at("version") != 1)
                        throw std::runtime_error("Unsupported material format: " + detail::pathUtf8(path));
                    // A superseded JSON file is not the active material payload.
                    if (preferredIds.contains(detail::readId(value.at("material").at("id")))) continue;
                    material = detail::readMaterial(value.at("material"));
                }
                detail::validateMaterial(material);
                if (isEngineAssetId(material.id) != (origin == AssetOrigin::Engine))
                    throw std::runtime_error("Material GUID is not permitted in this " +
                        std::string(origin == AssetOrigin::Engine ? "Engine" : "Project") +
                        " Content mount: " + detail::pathUtf8(path));
                material.origin = origin;
                if (!ids.insert(material.id).second)
                    throw std::runtime_error("Duplicate material GUID in directory: " + detail::pathUtf8(path));
                loaded.push_back(std::move(material));
            }
        }
        std::sort(loaded.begin(), loaded.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        return loaded;
}
} // namespace

bool AssetLibrary::loadMaterials(const std::filesystem::path& folder, std::string& error) {
    try {
        auto loaded = readMaterialFolder(folder, AssetOrigin::Project);
        std::unordered_set<AssetId> ids;
        for (const auto& material : loaded) ids.insert(material.id);
        for (const auto& material : materials) {
            if (material.origin != AssetOrigin::Engine) continue;
            if (!isEngineAssetId(material.id)) throw std::runtime_error("Engine material has an invalid reserved identity.");
            detail::validateMaterial(material);
            if (!ids.insert(material.id).second) throw std::runtime_error("Duplicate Engine material ID.");
            loaded.push_back(material);
        }
        materials = std::move(loaded);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool AssetLibrary::loadEngineMaterials(const std::filesystem::path& folder, std::string& error) {
    try {
        auto loaded = readMaterialFolder(folder, AssetOrigin::Engine);
        if (std::none_of(loaded.begin(), loaded.end(), [](const auto& material) {
            return material.id == EngineDefaultMaterialId;
        })) throw std::runtime_error("Engine Content is missing DefaultMaterial (GUID 0010000000000001): " + detail::pathUtf8(folder));
        std::unordered_set<AssetId> ids;
        for (const auto& material : loaded) ids.insert(material.id);
        for (const auto& material : materials) {
            if (material.origin == AssetOrigin::Engine) continue;
            if (isEngineAssetId(material.id)) throw std::runtime_error("Project material conflicts with a reserved Engine identity.");
            detail::validateMaterial(material);
            if (!ids.insert(material.id).second) throw std::runtime_error("Duplicate Project material ID.");
            loaded.push_back(material);
        }
        materials = std::move(loaded);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool AssetLibrary::loadEngineMeshes(const std::filesystem::path& folder, std::string& error) {
    try {
        if (!std::filesystem::is_directory(folder) || detail::contentLink(folder))
            throw std::runtime_error("Engine mesh directory is missing or linked: " + detail::pathUtf8(folder));
        std::vector<MeshAsset> loaded;
        std::unordered_set<AssetId> ids;
        for (const auto& path : detail::contentFiles(folder)) {
            if (!isAssetContainerFile(path)) continue;
            AssetFile file;
            if (!readAssetFileHeader(path, file, error)) throw std::runtime_error(error);
            if (file.type != AssetFileType::StaticMesh) continue;
            if (!isEngineAssetId(file.id)) throw std::runtime_error("Engine mesh must use a reserved identity.");
            if (!ids.insert(file.id).second) throw std::runtime_error("Duplicate Engine mesh GUID.");
            if (!readAssetFile(path, file, error)) throw std::runtime_error(error);
            MeshAsset mesh;
            if (!readMeshAssetFile(file, mesh, error)) throw std::runtime_error(error);
            mesh.origin = AssetOrigin::Engine;
            loaded.push_back(std::move(mesh));
        }
        for (const auto id : {EngineCubeMeshId, EngineSphereMeshId, EngineCylinderMeshId, EngineConeMeshId, EngineTorusMeshId, EnginePlaneMeshId})
            if (!ids.contains(id)) throw std::runtime_error("Engine Content is missing built-in mesh " + assetIdText(id));
        for (const auto& mesh : meshes) {
            if (mesh.origin == AssetOrigin::Engine) continue;
            if (isEngineAssetId(mesh.id)) throw std::runtime_error("Project mesh conflicts with a reserved Engine identity.");
            if (!ids.insert(mesh.id).second) throw std::runtime_error("Duplicate Project mesh ID.");
            loaded.push_back(mesh);
        }
        meshes = std::move(loaded);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

void AssetLibrary::pruneMeshPersistence() const {
    std::erase_if(persistedMeshes_, [this](const auto& entry) {
        const auto* mesh = findMesh(entry.first);
        return !mesh || mesh->origin != AssetOrigin::Project || mesh->primitive != MeshPrimitive::Imported ||
            mesh->geometryKey() != entry.second.geometry;
    });
}

void AssetLibrary::rememberMeshPersisted(AssetId id, const std::filesystem::path& path) const noexcept {
    // Erase first: a failed stat/allocation must not preserve an obsolete clean
    // baseline. The file/library transaction has already succeeded at this point.
    persistedMeshes_.erase(id);
    try {
        const auto* mesh = findMesh(id);
        if (!mesh || mesh->origin != AssetOrigin::Project || mesh->primitive != MeshPrimitive::Imported ||
            path.extension() != ".rasset" || !std::filesystem::is_regular_file(path)) return;
        MeshPersistence baseline;
        baseline.geometry = mesh->geometryKey();
        baseline.name = mesh->name;
        baseline.sourcePath = mesh->sourcePath;
        baseline.primitive = mesh->primitive;
        baseline.materialSlots = mesh->materialSlots;
        baseline.uvNames = mesh->uvNames;
        baseline.targetPath = pathKey(std::filesystem::weakly_canonical(std::filesystem::absolute(path)));
        baseline.size = std::filesystem::file_size(path);
        baseline.modified = std::filesystem::last_write_time(path);
        persistedMeshes_.emplace(id, std::move(baseline));
    } catch (...) { /* A missing baseline safely retries serialization next save. */ }
}

bool AssetLibrary::meshPersistenceMatches(const MeshAsset& mesh, const std::filesystem::path& path) const {
    const auto found = persistedMeshes_.find(mesh.id);
    if (found == persistedMeshes_.end()) return false;
    const auto& baseline = found->second;
    if (baseline.geometry != mesh.geometryKey() || baseline.name != mesh.name ||
        baseline.sourcePath != mesh.sourcePath || baseline.primitive != mesh.primitive ||
        baseline.materialSlots != mesh.materialSlots || baseline.uvNames != mesh.uvNames ||
        baseline.targetPath != pathKey(std::filesystem::weakly_canonical(std::filesystem::absolute(path)))) return false;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size != baseline.size) return false;
    const auto modified = std::filesystem::last_write_time(path, error);
    return !error && modified == baseline.modified;
}

bool AssetLibrary::saveMeshes(const std::filesystem::path& folder, std::string& error) const {
    try {
        pruneMeshPersistence();
        const auto existing = storedAssetPaths(folder, AssetFileType::StaticMesh);
        std::unordered_set<AssetId> ids;
        std::unordered_set<std::string> reserved;
        for (const auto& mesh : meshes) {
            if (!ids.insert(mesh.id).second) throw std::runtime_error("Duplicate mesh ID.");
            if (isEngineAssetId(mesh.id) != (mesh.origin == AssetOrigin::Engine))
                throw std::runtime_error("Mesh identity does not match its Engine/Project origin.");
        }
        for (const auto& mesh : meshes) {
            if (mesh.origin == AssetOrigin::Engine || mesh.primitive != MeshPrimitive::Imported) continue;
            const auto path = saveAssetPath(folder, meshAssetFileName(mesh), mesh.id, existing, reserved);
            if (meshPersistenceMatches(mesh, path)) continue;
            persistedMeshes_.erase(mesh.id);
            validateMeshForPersistence(mesh);
            auto file = makeMeshAssetFile(mesh);
            attachThumbnail(file, renderMeshThumbnail(mesh));
            if (!writeAssetFile(path, file, error)) return false;
            rememberMeshPersisted(mesh.id, path);
        }
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

std::string AssetLibrary::meshAssetFileName(const MeshAsset& mesh) {
    return sanitizeAssetName(mesh.name) + ".rasset";
}

bool AssetLibrary::loadMeshes(const std::filesystem::path& folder, std::string& error) {
    try {
        if (!std::filesystem::is_directory(folder)) { error.clear(); return true; }
        if (detail::contentLink(folder)) throw std::runtime_error("Mesh directory must not be linked.");
        auto staged = meshes;
        std::unordered_map<AssetId, std::filesystem::path> loadedPaths;
        std::unordered_set<AssetId> ids;
        const auto files = detail::contentFiles(folder);
        for (int pass = 0; pass < 2; ++pass) {
            const auto preferredIds = ids;
            for (const auto& path : files) {
                if (path.extension() != (pass == 0 ? ".rasset" : ".raasset")) continue;
                AssetFile file;
                if (!readAssetFileHeader(path, file, error)) throw std::runtime_error(error);
                if (file.type != AssetFileType::StaticMesh) continue;
                if (isEngineAssetId(file.id))
                    throw std::runtime_error("Project mesh conflicts with a reserved Engine identity: " + detail::pathUtf8(path));
                if (pass == 1 && preferredIds.contains(file.id)) continue;
                if (!ids.insert(file.id).second)
                    throw std::runtime_error("Duplicate mesh GUID in directory: " + detail::pathUtf8(path));
                if (!readAssetFile(path, file, error)) throw std::runtime_error(error);
                MeshAsset mesh;
                if (!readMeshAssetFile(file, mesh, error)) throw std::runtime_error(error);
                loadedPaths.emplace(mesh.id, path);
                const auto existing = std::find_if(staged.begin(), staged.end(),
                    [&mesh](const MeshAsset& candidate) { return candidate.id == mesh.id; });
                if (existing != staged.end()) *existing = std::move(mesh);
                else staged.push_back(std::move(mesh));
            }
        }
        meshes = std::move(staged);
        pruneMeshPersistence();
        for (const auto& [id, path] : loadedPaths) rememberMeshPersisted(id, path);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool AssetLibrary::ensureBuiltinMeshes(const std::filesystem::path& folder, std::string& error) const {
    try {
        const auto existing = storedAssetPaths(folder, AssetFileType::StaticMesh);
        std::unordered_set<std::string> reserved;
        for (const auto& mesh : meshes) {
            if (mesh.origin == AssetOrigin::Engine || mesh.primitive == MeshPrimitive::Imported) continue;
            const auto found = existing.find(mesh.id);
            if (found != existing.end()) {
                AssetFile file;
                std::string readError;
                if (readAssetFileHeader(found->second.path, file, readError) && file.manifest.contains("thumbnail")) continue;
            }
            auto file = makeMeshAssetFile(mesh);
            attachThumbnail(file, renderMeshThumbnail(mesh));
            const auto path = saveAssetPath(folder, meshAssetFileName(mesh), mesh.id, existing, reserved);
            if (!writeAssetFile(path, file, error)) return false;
        }
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
} // namespace rae::assets
