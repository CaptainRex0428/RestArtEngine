#include "RestArtEngine/Assets/AssetLibrary.h"
#include "RestArtEngine/Assets/AssetFile.h"
#include "AssetSerialization.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <sstream>

namespace rae::assets {
namespace {
using Vec3 = std::array<float, 3>;
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
    meshes.clear(); materials.clear(); nextMeshId_=4; nextMaterialId_=3;
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
    MaterialAsset material; material.id=detail::nextId(materials,nextMaterialId_);
    material.name=name.empty()?"Material":std::move(name);
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
        std::size_t textureCount=0,lineNumber=0;
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
                    float value=0; if (!(input>>value) || !std::isfinite(value)) throw std::runtime_error("Invalid OBJ texture coordinate.");
                    ++textureCount;
                } else if (command=="f") {
                    struct Corner {std::size_t position; std::size_t normal=0; bool hasNormal=false;};
                    std::vector<Corner> face; std::string token;
                    while (input>>token) {
                        const auto slash=token.find('/');
                        Corner corner{objIndex(token.substr(0,slash),positions.size())};
                        if (slash!=std::string::npos) {
                            const auto second=token.find('/',slash+1);
                            const auto texture=token.substr(slash+1,second==std::string::npos?std::string::npos:second-slash-1);
                            if (!texture.empty()) (void)objIndex(texture,textureCount);
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
                            mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
                            mesh.vertices.push_back({{p[0],p[1],p[2]},{n[0],n[1],n[2]}});
                        }
                    }
                }
                if (positions.size()>4000000 || mesh.vertices.size()>12000000) throw std::runtime_error("OBJ exceeds the editor mesh size limit.");
            } catch (const std::exception& e) {throw std::runtime_error("OBJ line " + std::to_string(lineNumber) + ": " + e.what());}
        }
        if (stream.bad()) throw std::runtime_error("Failed reading OBJ.");
        if (mesh.indices.empty()) throw std::runtime_error("OBJ contains no renderable faces.");
        mesh.id=detail::nextId(meshes,nextMeshId_);
        meshes.push_back(std::move(mesh));
        result=meshes.back().id; error.clear(); return true;
    } catch (const std::exception& e) {error=e.what();return false;}
}
bool AssetLibrary::saveMaterials(const std::filesystem::path& folder, std::string& error) const {
    try {
        std::unordered_set<AssetId> ids;
        std::vector<std::filesystem::path> written;
        for (const auto& material : materials) {
            if (!ids.insert(material.id).second) throw std::runtime_error("Duplicate material ID.");
            const auto path = folder / materialAssetFileName(material);
            if (!writeAssetFile(path, makeMaterialAssetFile(material), error)) return false;
            written.push_back(path);
        }
        // A renamed material writes a new file: drop the previous file that still
        // carries the same GUID, otherwise the next load reports a duplicate.
        std::error_code code;
        for (const auto& entry : std::filesystem::directory_iterator(folder, code)) {
            if (code) break;
            if (!entry.is_regular_file(code)) continue;
            const auto extension = entry.path().extension();
            if (extension != ".raasset" && extension != ".ramat") continue;
            if (std::find(written.begin(), written.end(), entry.path()) != written.end()) continue;
            AssetFile stale;
            std::string readError;
            if (extension == ".raasset") {
                if (!readAssetFileHeader(entry.path(), stale, readError)) continue;
                if (stale.type != AssetFileType::Material || !ids.contains(stale.id)) continue;
            } else {
                try {
                    const auto value = detail::readJson(entry.path());
                    if (value.at("format") != "RAEngine.Material") continue;
                    const auto material = detail::readMaterial(value.at("material"));
                    if (!ids.contains(material.id)) continue;
                } catch (const std::exception&) { continue; }
            }
            std::filesystem::remove(entry.path(), code);
        }
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

std::string AssetLibrary::materialAssetFileName(const MaterialAsset& material) {
    return sanitizeAssetName(material.name) + ".raasset";
}

bool AssetLibrary::loadMaterials(const std::filesystem::path& folder, std::string& error) {
    try {
        if (!std::filesystem::is_directory(folder))
            throw std::runtime_error("Material directory does not exist: " + detail::pathUtf8(folder));
        std::vector<MaterialAsset> loaded;
        std::unordered_set<AssetId> ids;
        std::vector<std::filesystem::path> legacy;
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (!entry.is_regular_file()) continue;
            const auto extension = entry.path().extension();
            if (extension == ".raasset") {
                AssetFile file;
                if (!readAssetFileHeader(entry.path(), file, error)) throw std::runtime_error(error);
                if (file.type != AssetFileType::Material) continue;
                if (!readAssetFile(entry.path(), file, error)) throw std::runtime_error(error);
                MaterialAsset material;
                if (!readMaterialAssetFile(file, material, error)) throw std::runtime_error(error);
                if (!ids.insert(material.id).second)
                    throw std::runtime_error("Duplicate material GUID in directory: " + detail::pathUtf8(entry.path()));
                loaded.push_back(std::move(material));
            } else if (extension == ".ramat") {
                legacy.push_back(entry.path());
            }
        }
        // Legacy .ramat files load only when the same GUID is not already present
        // as a .raasset; the next save writes the new container.
        for (const auto& path : legacy) {
            const auto value = detail::readJson(path);
            if (value.at("format") != "RAEngine.Material" || value.at("version") != 1)
                throw std::runtime_error("Unsupported material format: " + detail::pathUtf8(path));
            auto material = detail::readMaterial(value.at("material"));
            if (!ids.insert(material.id).second) continue;
            loaded.push_back(std::move(material));
        }
        if (loaded.empty()) throw std::runtime_error("No .raasset or .ramat materials found.");
        std::sort(loaded.begin(), loaded.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        materials = std::move(loaded);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool AssetLibrary::saveMeshes(const std::filesystem::path& folder, std::string& error) const {
    try {
        std::unordered_set<AssetId> ids;
        std::vector<std::filesystem::path> written;
        for (const auto& mesh : meshes) {
            // Built-in primitives are engine data, not project assets.
            if (mesh.primitive != MeshPrimitive::Imported) continue;
            if (!ids.insert(mesh.id).second) throw std::runtime_error("Duplicate mesh ID.");
            const auto path = folder / meshAssetFileName(mesh);
            if (!writeAssetFile(path, makeMeshAssetFile(mesh), error)) return false;
            written.push_back(path);
        }
        std::error_code code;
        for (const auto& entry : std::filesystem::directory_iterator(folder, code)) {
            if (code) break;
            if (!entry.is_regular_file(code) || entry.path().extension() != ".raasset") continue;
            if (std::find(written.begin(), written.end(), entry.path()) != written.end()) continue;
            AssetFile stale;
            std::string readError;
            if (!readAssetFileHeader(entry.path(), stale, readError)) continue;
            if (stale.type != AssetFileType::StaticMesh || !ids.contains(stale.id)) continue;
            std::filesystem::remove(entry.path(), code);
        }
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

std::string AssetLibrary::meshAssetFileName(const MeshAsset& mesh) {
    return sanitizeAssetName(mesh.name) + ".raasset";
}

bool AssetLibrary::loadMeshes(const std::filesystem::path& folder, std::string& error) {
    try {
        if (!std::filesystem::is_directory(folder)) { error.clear(); return true; }
        std::vector<MeshAsset> loaded;
        std::unordered_set<AssetId> ids;
        for (const auto& mesh : meshes) ids.insert(mesh.id);
        for (const auto& entry : std::filesystem::recursive_directory_iterator(folder)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".raasset") continue;
            AssetFile file;
            if (!readAssetFileHeader(entry.path(), file, error)) throw std::runtime_error(error);
            if (file.type != AssetFileType::StaticMesh) continue;
            if (!readAssetFile(entry.path(), file, error)) throw std::runtime_error(error);
            MeshAsset mesh;
            if (!readMeshAssetFile(file, mesh, error)) throw std::runtime_error(error);
            const auto existing = std::find_if(meshes.begin(), meshes.end(),
                [&mesh](const MeshAsset& candidate) { return candidate.id == mesh.id; });
            if (existing != meshes.end()) {
                // A file for a built-in primitive replaces the procedural copy; a
                // second file for an imported mesh is a duplicate.
                if (existing->primitive == MeshPrimitive::Imported)
                    throw std::runtime_error("Duplicate mesh GUID in directory: " + detail::pathUtf8(entry.path()));
                *existing = std::move(mesh);
                continue;
            }
            if (!ids.insert(mesh.id).second)
                throw std::runtime_error("Duplicate mesh GUID in directory: " + detail::pathUtf8(entry.path()));
            loaded.push_back(std::move(mesh));
        }
        meshes.insert(meshes.end(), std::make_move_iterator(loaded.begin()), std::make_move_iterator(loaded.end()));
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool AssetLibrary::ensureBuiltinMeshes(const std::filesystem::path& folder, std::string& error) const {
    try {
        std::unordered_set<AssetId> present;
        std::error_code code;
        if (std::filesystem::is_directory(folder, code)) {
            for (const auto& entry : std::filesystem::directory_iterator(folder, code)) {
                if (code || !entry.is_regular_file(code) || entry.path().extension() != ".raasset") continue;
                AssetFile file;
                std::string readError;
                if (!readAssetFileHeader(entry.path(), file, readError)) continue;
                if (file.type == AssetFileType::StaticMesh) present.insert(file.id);
            }
        }
        for (const auto& mesh : meshes) {
            if (mesh.primitive == MeshPrimitive::Imported) continue;
            if (present.contains(mesh.id)) continue;
            if (!writeAssetFile(folder / meshAssetFileName(mesh), makeMeshAssetFile(mesh), error)) return false;
        }
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
} // namespace rae::assets
