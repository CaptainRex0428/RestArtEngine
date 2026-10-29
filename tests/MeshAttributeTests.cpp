#include "RestArtEngine/Assets/AssetFile.h"
#include "RestArtEngine/Assets/AssetImport.h"
#include "../src/Assets/FbxImporter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace rae::assets;
namespace fs = std::filesystem;
using Json = nlohmann::json;
unsigned checks = 0;
void require(bool value, const std::string& message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
struct Files {
    fs::path root = fs::temp_directory_path() / ("rae_mesh_attributes_" + assetIdText(generateAssetId()));
    Files() { fs::create_directories(root); }
    ~Files() {
        std::error_code ignored;
        if (root.parent_path() == fs::temp_directory_path() && root.filename().string().starts_with("rae_mesh_attributes_"))
            fs::remove_all(root, ignored);
    }
};
void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary); stream << text;
    stream.close(); require(bool(stream), "Write fixture.");
}
std::string bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::string fbx(unsigned channels, bool instance = true) {
    std::string source = R"fbx(; FBX 7.4.0 project file
FBXHeaderExtension: { FBXHeaderVersion: 1003
 FBXVersion: 7400
}
GlobalSettings: { Version: 1000
 Properties70: {
  P: "UpAxis", "int", "Integer", "", 1
  P: "UpAxisSign", "int", "Integer", "", 1
  P: "FrontAxis", "int", "Integer", "", 2
  P: "FrontAxisSign", "int", "Integer", "", -1
  P: "CoordAxis", "int", "Integer", "", 0
  P: "CoordAxisSign", "int", "Integer", "", 1
  P: "UnitScaleFactor", "double", "Number", "", 100
 }
}
Objects: {
 Geometry: 100, "Geometry::Quad", "Mesh" {
  Vertices: *12 { a: 0,0,0, 1,0,0, 1,1,0, 0,1,0 }
  PolygonVertexIndex: *6 { a: 0,1,-3, 0,2,-4 }
  LayerElementMaterial: 0 {
   Version: 101
   Name: ""
   MappingInformationType: "ByPolygon"
   ReferenceInformationType: "IndexToDirect"
   Materials: *2 { a: 0,1 }
  }
)fbx";
    for (unsigned channel = 0; channel < channels; ++channel) {
        const auto c = std::to_string(channel);
        source += "  LayerElementUV: " + c + " {\n   Version: 101\n   Name: \"UV" + c + "\"\n";
        source += "   MappingInformationType: \"ByPolygonVertex\"\n   ReferenceInformationType: \"IndexToDirect\"\n";
        source += "   UV: *12 { a: " + c + ".1,0.2, " + c + ".3,0.4, " + c + ".5,0.6, " + c + ".7,0.8, " + c + ".9,1.0, " + c + ".2,1.2 }\n";
        source += "   UVIndex: *6 { a: 0,1,2,3,4,5 }\n  }\n";
    }
    for (unsigned channel = 0; channel < std::max(1u, channels); ++channel) {
        source += "  Layer: " + std::to_string(channel) + " {\n   Version: 100\n";
        if (!channel) source += "   LayerElement: { Type: \"LayerElementMaterial\"\n    TypedIndex: 0\n   }\n";
        if (channel < channels) source += "   LayerElement: { Type: \"LayerElementUV\"\n    TypedIndex: " + std::to_string(channel) + "\n   }\n";
        source += "  }\n";
    }
    source += R"fbx( }
 Model: 200, "Model::First", "Mesh" { Version: 232 }
 Material: 300, "Material::Paint", "" { Version: 102
  ShadingModel: "phong"
 }
 Material: 301, "Material::Paint", "" { Version: 102
  ShadingModel: "phong"
 }
)fbx";
    if (instance) source += R"fbx( Model: 201, "Model::Mirror", "Mesh" { Version: 232
  Properties70: {
   P: "Lcl Translation", "Lcl Translation", "", "A", 3,0,0
   P: "Lcl Scaling", "Lcl Scaling", "", "A", -1,1,1
  }
 }
)fbx";
    source += "}\nConnections: {\n C: \"OO\", 100,200\n C: \"OO\", 200,0\n C: \"OO\", 300,200\n C: \"OO\", 301,200\n";
    if (instance) source += " C: \"OO\", 100,201\n C: \"OO\", 201,0\n C: \"OO\", 301,201\n C: \"OO\", 300,201\n";
    source += "}\n";
    return source;
}
MeshAsset importedFbx(const fs::path& path, unsigned channels, MeshImportDiagnostics& diagnostics) {
    write(path, fbx(channels));
    MeshAsset mesh; std::string error;
    const bool parsed = detail::readFbxMesh(path, mesh, error, &diagnostics);
    require(parsed, "Read FBX fixture: " + error);
    mesh.id = 91;
    return mesh;
}
void requireAttributes(const MeshAsset& expected, const MeshAsset& actual) {
    require(expected.materialSlots == actual.materialSlots && expected.sections == actual.sections,
        "Slots and sections survive self-contained persistence.");
    require(expected.uvChannels == actual.uvChannels && expected.uvNames == actual.uvNames,
        "All UV values and names survive self-contained persistence.");
}
void invalidFile(const AssetFile& file) {
    MeshAsset survivor; survivor.id = 733; survivor.name = "Unchanged";
    const auto key = survivor.geometryKey(); std::string error;
    require(!readMeshAssetFile(file, survivor, error) && !error.empty(), "Reject invalid attribute payload.");
    require(survivor.id == 733 && survivor.name == "Unchanged" && survivor.geometryKey() == key,
        "Invalid attribute loading is transactional.");
}
// Read-only source inspection for large local regression fixtures. The config
// carries the private source path; reports intentionally omit that path and no
// source, asset container, thumbnail, or temporary project is written.
void inspectSource(const fs::path& configuration) {
    std::ifstream input(configuration, std::ios::binary);
    require(bool(input), "Cannot open source inspection configuration.");
    const auto config = Json::parse(input);
    const auto source = config.at("source").get<std::string>();
    const fs::path path{std::u8string(source.begin(), source.end())};
    MeshAsset mesh; MeshImportDiagnostics diagnostics; std::string error;
    const bool imported = detail::readFbxMesh(path, mesh, error, &diagnostics);
    require(imported, "FBX source inspection failed: " + error);
    require(!mesh.vertices.empty() && !mesh.indices.empty() && mesh.indices.size() % 3 == 0,
        "Inspected source has invalid triangle geometry.");
    require(mesh.localBounds().valid, "Inspected source has invalid bounds.");
    for (const auto index : mesh.indices)
        if (index >= mesh.vertices.size()) throw std::runtime_error("Inspected source index is invalid.");
    for (const auto& vertex : mesh.vertices) {
        double normalLength = 0;
        for (unsigned axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(vertex.position[axis]) || !std::isfinite(vertex.normal[axis]))
                throw std::runtime_error("Inspected source contains non-finite geometry.");
            normalLength += double(vertex.normal[axis]) * vertex.normal[axis];
        }
        if (!(normalLength > 0)) throw std::runtime_error("Inspected source contains a zero normal.");
    }
    std::size_t nextIndex = 0;
    for (const auto& section : mesh.sections) {
        require(section.firstIndex == nextIndex && section.indexCount && section.indexCount % 3 == 0 &&
            section.materialSlot < mesh.materialSlotCount() && section.indexCount <= mesh.indices.size() - nextIndex,
            "Inspected source has an invalid material section.");
        nextIndex += section.indexCount;
    }
    require(mesh.sections.empty() || nextIndex == mesh.indices.size(), "Inspected sections do not cover the mesh.");
    Json uv = Json::array(), slots = Json::array(), sections = Json::array();
    for (unsigned channel = 0; channel < MaxMeshUvChannels; ++channel) {
        const auto& stream = mesh.uvChannels[channel];
        if (stream.empty()) continue;
        require(stream.size() == mesh.vertices.size(), "Inspected UV count does not match the vertex stream.");
        for (const auto& value : stream)
            if (!std::isfinite(value[0]) || !std::isfinite(value[1]))
                throw std::runtime_error("Inspected UV stream is not finite.");
        uv.push_back({{"channel",channel},{"name",mesh.uvNames[channel]},{"count",stream.size()}});
    }
    for (const auto& slot : mesh.materialSlots)
        slots.push_back({{"name",slot.name},{"sourceKey",slot.sourceKey}});
    for (const auto& section : mesh.sections)
        sections.push_back({{"materialSlot",section.materialSlot},{"firstIndex",section.firstIndex},{"indexCount",section.indexCount}});
    const Json report{{"passed",true},{"vertices",mesh.vertices.size()},{"triangles",mesh.indices.size()/3},
        {"slotCount",mesh.materialSlotCount()},{"slots",slots},{"sectionCount",mesh.sections.size()},{"sections",sections},
        {"uvChannelCount",uv.size()},{"uvChannelMask",mesh.uvChannelMask()},{"uvChannels",uv},
        {"diagnostics",{{"skippedDegenerateTriangles",diagnostics.skippedDegenerateTriangles},
            {"repairedNormals",diagnostics.repairedNormals},{"droppedUvChannels",diagnostics.droppedUvChannels}}}};
    std::cout << report.dump(2) << '\n';
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--inspect-source-json") {
            const std::string argument = argv[2];
            inspectSource(fs::path{std::u8string(argument.begin(), argument.end())});
            return 0;
        }
        if (argc != 1) throw std::runtime_error("Usage: MeshAttributeTests [--inspect-source-json <configuration.json>]");
        Files files; std::string error;
        static_assert(sizeof(MeshVertex) == 24);
        MeshImportDiagnostics diagnostics;
        auto mesh = importedFbx(files.root / "four.fbx", 4, diagnostics);
        require(mesh.vertices.size() == 12 && mesh.indices.size() == 12 && mesh.sections.size() == 2,
            "Shared geometry instances produce four triangles grouped into two sections.");
        require(mesh.materialSlots.size() == 2 && mesh.materialSlots[0].name == mesh.materialSlots[1].name &&
            mesh.materialSlots[0].sourceKey != mesh.materialSlots[1].sourceKey,
            "Same-name distinct source materials retain distinct slot identities.");
        require(mesh.uvChannelMask() == 15 && diagnostics.droppedUvChannels == 0, "Exactly four UV channels retained.");
        for (unsigned channel = 0; channel < 4; ++channel) {
            require(mesh.uvChannels[channel].size() == mesh.vertices.size(), "UV stream matches final corner vertices.");
            require(std::abs(mesh.uvChannels[channel][0][0] - (channel + .1f)) < .00001f &&
                std::abs(mesh.uvChannels[channel][3][0] - (channel + .7f)) < .00001f,
                "Source corners with the same position retain different seam UV values.");
        }
        for (const auto& section : mesh.sections) {
            require(section.indexCount == 6 && section.firstIndex % 3 == 0, "Material sections are coalesced.");
            const auto a = mesh.indices[section.firstIndex], b = mesh.indices[section.firstIndex + 3];
            require((a < 6) != (b < 6), "Each section includes the matching material from both instances.");
            const auto localA = a / 3, localB = (b - 6) / 3;
            require(localA != localB, "Instance-specific node material ordering is respected.");
        }
        {
            auto source = fbx(1);
            const auto objectsEnd = source.find("}\nConnections:");
            require(objectsEnd != std::string::npos, "Locate fixture object table.");
            source.insert(objectsEnd, " Material: 302, \"Material::Unused\", \"\" { Version: 102\n  ShadingModel: \"phong\"\n }\n");
            const std::string connection = " C: \"OO\", 301,200\n";
            const auto connectionEnd = source.find(connection);
            require(connectionEnd != std::string::npos, "Locate fixture material connections.");
            source.insert(connectionEnd + connection.size(), " C: \"OO\", 302,200\n");
            write(files.root / "unused.fbx", source);
            MeshAsset unused;
            const bool parsed = detail::readFbxMesh(files.root / "unused.fbx", unused, error);
            require(parsed, "Read unused slot fixture: " + error);
            require(unused.materialSlots.size() == 3 && unused.materialSlots[2].name == "Unused" && unused.sections.size() == 2,
                "Declared unused source material slots remain available without zero-length draw sections.");
        }
        {
            auto source = fbx(1, false);
            const std::string original = "Material: 301, \"Material::Paint\"";
            const auto secondName = source.find(original);
            require(secondName != std::string::npos, "Locate unique-material fixture name.");
            source.replace(secondName, original.size(), "Material: 301, \"Material::Glass\"");
            write(files.root / "unique.fbx", source);
            MeshAsset unique;
            bool parsed = detail::readFbxMesh(files.root / "unique.fbx", unique, error);
            require(parsed, "Read unique-material fixture: " + error);
            require(unique.materialSlots.size() == 2 && unique.materialSlots[0].sourceKey == "fbx:Paint:0",
                "A uniquely connected material starts with its stable zero ordinal.");
            const auto materialBegin = source.find(" Material: 300,");
            require(materialBegin != std::string::npos, "Locate source material table.");
            source.insert(materialBegin, " Material: 299, \"Material::Paint\", \"\" { Version: 102\n  ShadingModel: \"phong\"\n }\n");
            write(files.root / "orphan-material.fbx", source);
            MeshAsset withOrphan;
            parsed = detail::readFbxMesh(files.root / "orphan-material.fbx", withOrphan, error);
            require(parsed, "Read orphan-material fixture: " + error);
            require(withOrphan.materialSlots == unique.materialSlots,
                "Unconnected same-name material records do not change bound slot identities.");
        }
        auto five = importedFbx(files.root / "five.fbx", 5, diagnostics);
        require(five.uvChannelMask() == 15 && diagnostics.droppedUvChannels == 1, "Fifth UV is explicitly reported and truncated.");
        const auto file = makeMeshAssetFile(mesh);
        require(file.manifest["payload"]["vertexStride"] == 24 && file.manifest["payload"]["uvChannels"].size() == 4,
            "UVs use optional blobs alongside the unchanged legacy vertex blob.");
        MeshAsset loaded;
        require(readMeshAssetFile(file, loaded, error), error); requireAttributes(mesh, loaded);
        require(writeAssetFile(files.root / "mesh.rasset", file, error), error);
        AssetFile disk;
        require(readAssetFile(files.root / "mesh.rasset", disk, error) && readMeshAssetFile(disk, loaded, error), error);
        requireAttributes(mesh, loaded);
        {
            AssetLibrary builtins; builtins.createDefaults();
            const auto legacy = makeMeshAssetFile(*builtins.findMesh(3));
            require(!legacy.manifest["payload"].contains("materialSlots") && !legacy.manifest["payload"].contains("uvChannels"),
                "Legacy/no-attribute assets retain the original manifest shape.");
            require(readMeshAssetFile(legacy, loaded, error), error);
            require(loaded.materialSlotCount() == 1 && loaded.sections.empty() && loaded.uvChannelMask() == 0,
                "Legacy assets have an implicit single material and no UV allocation.");
        }
        for (int mutation = 0; mutation < 15; ++mutation) {
            auto bad = file; auto& payload = bad.manifest["payload"];
            switch (mutation) {
            case 0: payload["sections"][0]["indexCount"] = 1; break;
            case 1: payload["sections"][1]["firstIndex"] = 0; break;
            case 2: payload["sections"][0]["materialSlot"] = 2; break;
            case 3: payload["uvChannels"][0]["channel"] = 4; break;
            case 4: payload["uvChannels"][1]["channel"] = 0; break;
            case 5: payload["uvChannels"][0]["blob"]["size"] = 8; break;
            case 6: payload["materialSlots"][0]["defaultMaterialId"] = -1; break;
            case 7: payload["sections"][0]["firstIndex"] = 0.5; break;
            case 8: payload["vertexCount"] = std::numeric_limits<std::uint64_t>::max(); break;
            case 10: payload["meshAttributesVersion"] = 2; break;
            case 11: payload["meshAttributesVersion"] = 1.0; break;
            case 12: payload["uvChannels"][0]["blob"]["offset"] = 0.5; break;
            case 13:
                payload["materialSlots"][0]["sourceKey"] = "";
                payload["materialSlots"][1]["sourceKey"] = "";
                break;
            case 14:
                payload["materialSlots"][0]["sourceKey"] = "";
                payload["materialSlots"][1]["sourceKey"] = payload["materialSlots"][0]["name"];
                break;
            case 9: {
                const auto offset = payload["uvChannels"][0]["blob"]["offset"].get<std::size_t>();
                const float nan = std::numeric_limits<float>::quiet_NaN();
                std::memcpy(bad.blobs.data() + offset, &nan, sizeof(nan)); break;
            }
            }
            invalidFile(bad);
        }
        {
            auto duplicateKeys = mesh;
            duplicateKeys.materialSlots[0].sourceKey.clear();
            duplicateKeys.materialSlots[1].sourceKey.clear();
            bool refused = false;
            try { (void)makeMeshAssetFile(duplicateKeys); } catch (const std::exception&) { refused = true; }
            require(refused, "Saving rejects duplicate effective name keys before publishing an unloadable asset.");
            duplicateKeys.materialSlots[1].sourceKey = duplicateKeys.materialSlots[0].name;
            refused = false;
            try { (void)makeMeshAssetFile(duplicateKeys); } catch (const std::exception&) { refused = true; }
            require(refused, "Saving rejects collisions between sourceKey and fallback name identities.");
        }
        auto invalid = mesh; invalid.uvChannels[0].pop_back();
        bool rejected = false;
        try { (void)makeMeshAssetFile(invalid); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Writing rejects mismatched UV stream length.");

        const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt .1 .2\nvt .3 .4\nvt .5 .6\nvt 2 -1\n"
            "usemtl First paint\nf 1/1 2/2 3/3\nusemtl Second\nf -3/-1 -2/2 -1/3\n"
            "usemtl First paint\nf 1/1 2/2 3/3\n";
        write(files.root / "seams.obj", obj);
        AssetLibrary library; AssetId id = 0;
        require(library.importObj(files.root / "seams.obj", id, error), error);
        const auto* objMesh = library.findMesh(id);
        require(objMesh && objMesh->materialSlots.size() == 2 && objMesh->sections.size() == 2 &&
            objMesh->sections[0].indexCount == 6 && objMesh->sections[1].indexCount == 3,
            "OBJ usemtl groups disjoint face runs into material sections.");
        require(objMesh->uvChannels[0][3] == std::array<float,2>{2,-1} && objMesh->uvChannelMask() == 1,
            "OBJ negative UV indices and out-of-unit-range coordinates are retained.");
        const auto count = library.meshes.size();
        write(files.root / "invalid.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nf 1/2 2/1 3/1\n");
        require(!library.importObj(files.root / "invalid.obj", id, error) && library.meshes.size() == count,
            "Bad OBJ UV reference does not publish a partial mesh.");

        const auto content = files.root / "Content";
        const std::array sources{files.root / "seams.obj"};
        AssetLibrary persistent;
        auto report = importAssets(persistent, content, "Meshes", sources);
        require(report.importedCount() == 1, report.summary());
        const auto published = report.items.front().assetPath;
        auto* stored = persistent.findMesh(report.items.front().id);
        const auto originalBytes = bytes(published);
        stored->materialSlots[0].defaultMaterialId = 101;
        stored->uvNames[0] = "Paint UV";
        require(persistent.saveMeshes(content / "Meshes", error), error);
        require(bytes(published) != originalBytes, "Slot metadata and UV names invalidate the persisted clean baseline.");
        require(readAssetFile(published, disk, error) && readMeshAssetFile(disk, loaded, error), error);
        require(loaded.materialSlots[0].defaultMaterialId == 101 && loaded.uvNames[0] == "Paint UV",
            "Edited slot default and UV name are saved.");
        stored->uvChannels[0][0] = {7,8}; stored->markGeometryModified();
        require(persistent.saveMeshes(content / "Meshes", error), error);
        require(readAssetFile(published, disk, error) && readMeshAssetFile(disk, loaded, error), error);
        require(loaded.uvChannels[0][0] == std::array<float,2>{7,8}, "In-place UV revision invalidates persistence.");
        write(files.root / "seams.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl Second\nf 1 2 3\nusemtl First paint\nf 1 2 3\n");
        const auto reimported = reimportAsset(persistent, content, "Meshes/" + published.filename().string());
        require(reimported.succeeded(), reimported.error);
        stored = persistent.findMesh(reimported.id);
        require(stored->materialSlots[0].name == "Second" && stored->materialSlots[1].defaultMaterialId == 101,
            "Reimport preserves material defaults by source identity across slot reordering.");
        const std::array fbxSources{files.root / "five.fbx"};
        report = importAssets(persistent, content, "Meshes", fbxSources);
        require(report.importedCount() == 1 && report.warningCount() >= 1,
            "The public import report surfaces UV channel truncation warnings.");
        const auto ambiguousAsset = report.items.front();
        persistent.findMesh(ambiguousAsset.id)->materialSlots[0].defaultMaterialId = 202;
        require(persistent.saveMeshes(content / "Meshes", error), error);
        const auto beforeAmbiguous = bytes(ambiguousAsset.assetPath);
        const auto beforeAmbiguousKey = persistent.findMesh(ambiguousAsset.id)->geometryKey();
        const auto ambiguous = reimportAsset(persistent, content, "Meshes/" + ambiguousAsset.assetPath.filename().string());
        require(!ambiguous.succeeded() && ambiguous.error.find("duplicate FBX material names") != std::string::npos &&
            bytes(ambiguousAsset.assetPath) == beforeAmbiguous && persistent.findMesh(ambiguousAsset.id)->geometryKey() == beforeAmbiguousKey &&
            persistent.findMesh(ambiguousAsset.id)->materialSlots[0].defaultMaterialId == 202,
            "Ambiguous duplicate-name FBX reimport refuses to exchange bindings and leaves asset/memory unchanged.");
        std::cout << "PASS: " << checks << " mesh attribute checks (slots, seams, four UVs, persistence and reimport).\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "MeshAttributeTests: " << failure.what() << '\n';
        return 1;
    }
}
