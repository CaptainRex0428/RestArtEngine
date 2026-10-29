#include "RestArtEngine/Assets/AssetFile.h"
#include "../src/Assets/AssetSerialization.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace rae::assets;
using Json = nlohmann::json;
namespace fs = std::filesystem;
unsigned checks = 0;

void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

MaterialAsset fixture() {
    MaterialAsset material;
    material.id = 73;
    material.name = "Paint";
    material.baseColor[0] = .3f;
    material.baseColor[1] = .6f;
    material.baseColor[2] = .9f;
    material.roughness = .45f;
    material.metallic = .1f;
    material.grid = true;
    return material;
}

Json state(const MaterialAsset& material) {
    auto value = detail::materialJson(material);
    value["origin"] = static_cast<int>(material.origin);
    return value;
}

void roundTrips() {
    constexpr std::array culls{MaterialCullMode::Off, MaterialCullMode::Front, MaterialCullMode::Back};
    constexpr std::array depths{MaterialDepthTest::Never, MaterialDepthTest::Less, MaterialDepthTest::Equal,
        MaterialDepthTest::LessEqual, MaterialDepthTest::Greater, MaterialDepthTest::NotEqual,
        MaterialDepthTest::GreaterEqual, MaterialDepthTest::Always, MaterialDepthTest::Disabled};
    for (const auto cull : culls) for (const auto depth : depths) for (const bool write : {false, true}) {
        auto material = fixture();
        material.setCullMode(cull);
        material.depthTest = depth;
        material.depthWrite = write;
        MaterialAsset loaded;
        std::string error;
        const auto file = makeMaterialAssetFile(material);
        require(file.manifest["payload"]["cullMode"].is_string() &&
            file.manifest["payload"]["depthTest"].is_string(), "Render-state names must be portable strings.");
        require(readMaterialAssetFile(file, loaded, error), "Read material container: " + error);
        require(state(loaded) == state(material), "Modern material round trip changed a field.");
        const auto legacy = detail::readMaterial(detail::materialJson(material));
        require(state(legacy) == state(material), "Legacy material/scene payload round trip changed a field.");
        require(loaded.twoSided == (cull == MaterialCullMode::Off) &&
            loaded.cullFront == (cull == MaterialCullMode::Front), "Cull state is not canonical.");
        require(loaded.hardwareDepthCompatible() == (write &&
            (depth == MaterialDepthTest::Less || depth == MaterialDepthTest::LessEqual)),
            "Hardware compatibility did not reflect raster depth visibility.");
    }
}

void compatibility() {
    const auto material = fixture();
    auto modern = makeMaterialAssetFile(material);
    auto& payload = modern.manifest["payload"];
    for (const auto* field : {"twoSided", "cullMode", "depthTest", "depthWrite"}) payload.erase(field);
    MaterialAsset loaded;
    std::string error;
    require(readMaterialAssetFile(modern, loaded, error), error);
    require(loaded.effectiveCullMode() == MaterialCullMode::Back &&
        loaded.depthTest == MaterialDepthTest::Less && loaded.depthWrite,
        "Old modern containers must keep single-sided/Less/Write defaults.");

    auto legacy = detail::materialJson(material);
    for (const auto* field : {"twoSided", "cullMode", "depthTest", "depthWrite"}) legacy.erase(field);
    loaded = detail::readMaterial(legacy);
    require(loaded.effectiveCullMode() == MaterialCullMode::Off &&
        loaded.depthTest == MaterialDepthTest::Less && loaded.depthWrite,
        "Old .ramat and embedded scenes must retain two-sided visibility.");
    for (const bool twoSided : {false, true}) {
        payload["twoSided"] = twoSided;
        require(readMaterialAssetFile(modern, loaded, error), error);
        require(loaded.twoSided == twoSided && !loaded.cullFront,
            "Existing modern twoSided values must remain authoritative without cullMode.");
        legacy["twoSided"] = twoSided;
        loaded = detail::readMaterial(legacy);
        require(loaded.twoSided == twoSided && !loaded.cullFront, "Existing legacy twoSided values changed.");
    }
    payload.erase("twoSided");
    payload["cullMode"] = "front";
    require(readMaterialAssetFile(modern, loaded, error) && loaded.effectiveCullMode() == MaterialCullMode::Front,
        "Explicit modern cullMode must work without the compatibility field.");
    legacy.erase("twoSided");
    legacy["cullMode"] = "front";
    require(detail::readMaterial(legacy).effectiveCullMode() == MaterialCullMode::Front,
        "Explicit legacy cullMode must override its missing-field default.");
}

struct TemporaryTree {
    fs::path root = fs::temp_directory_path() / ("rae_material_state_" + assetIdText(generateAssetId()));
    TemporaryTree() { require(fs::create_directory(root), "Create isolated material test tree."); }
    ~TemporaryTree() {
        std::error_code ignored;
        if (root.parent_path() == fs::temp_directory_path() && root.filename().string().starts_with("rae_material_state_"))
            fs::remove_all(root, ignored);
    }
};

void rejectedInputsPreserveState() {
    const auto valid = makeMaterialAssetFile(fixture());
    auto retained = fixture();
    retained.name = "Existing";
    retained.id = 99;
    retained.setCullMode(MaterialCullMode::Front);
    retained.depthTest = MaterialDepthTest::Greater;
    retained.depthWrite = false;
    retained.origin = AssetOrigin::Engine;
    const auto before = state(retained);
    const auto reject = [&](const char* field, const Json& value, bool conflict = false) {
        auto invalid = valid;
        invalid.manifest["payload"][field] = value;
        if (conflict) invalid.manifest["payload"]["twoSided"] = value != "off";
        std::string error;
        require(!readMaterialAssetFile(invalid, retained, error) && !error.empty(),
            std::string("Invalid ") + field + " was accepted.");
        require(state(retained) == before, "A failed material decode partially overwrote its output.");
        auto legacy = invalid.manifest["payload"];
        legacy["id"] = fixture().id;
        legacy["name"] = fixture().name;
        bool threw = false;
        try { (void)detail::readMaterial(legacy); } catch (const std::exception&) { threw = true; }
        require(threw, std::string("Legacy decoder accepted invalid ") + field);
    };
    for (const auto* field : {"cullMode", "depthTest"}) {
        for (const Json& bad : {Json(nullptr), Json(true), Json(4), Json::array(), Json("typo")}) reject(field, bad);
    }
    for (const auto* field : {"twoSided", "depthWrite"})
        for (const Json& bad : {Json(nullptr), Json(1), Json("false"), Json::array()}) reject(field, bad);
    for (const char* cull : {"off", "front", "back"}) reject("cullMode", Json(cull), true);

    TemporaryTree tree;
    auto invalid = valid;
    invalid.manifest["payload"]["depthTest"] = "unknown";
    std::string error;
    require(writeAssetFile(tree.root / "Invalid.rasset", invalid, error), error);
    AssetLibrary library;
    library.materials.push_back(fixture());
    const auto libraryBefore = state(library.materials.front());
    require(!library.loadMaterials(tree.root, error) && !error.empty() && library.materials.size() == 1 &&
        state(library.materials.front()) == libraryBefore, "Invalid modern state partially replaced the asset library.");
    fs::remove(tree.root / "Invalid.rasset");
    auto legacy = detail::materialJson(fixture());
    legacy["depthWrite"] = "false";
    std::ofstream stream(tree.root / "Invalid.ramat", std::ios::binary);
    stream << Json{{"format", "RAEngine.Material"}, {"version", 1}, {"material", legacy}}.dump();
    stream.close();
    require(bool(stream), "Write isolated legacy failure fixture.");
    require(!library.loadMaterials(tree.root, error) && !error.empty() && library.materials.size() == 1 &&
        state(library.materials.front()) == libraryBefore, "Invalid legacy state partially replaced the asset library.");
}

void cloneAndPersist() {
    AssetLibrary library;
    auto source = fixture();
    source.setCullMode(MaterialCullMode::Front);
    source.depthTest = MaterialDepthTest::NotEqual;
    source.depthWrite = false;
    library.materials.push_back(source);
    const auto id = library.cloneMaterial(source.id, "Independent paint");
    require(id != source.id && library.materials.size() == 2, "Cloning must allocate a separate asset identity.");
    auto* copy = library.findMaterial(id);
    require(copy && copy->effectiveCullMode() == MaterialCullMode::Front &&
        copy->depthTest == MaterialDepthTest::NotEqual && !copy->depthWrite, "Clone lost material render state.");
    copy->setCullMode(MaterialCullMode::Off);
    copy->depthTest = MaterialDepthTest::Always;
    copy->depthWrite = true;
    require(state(*library.findMaterial(source.id)) == state(source), "Editing a copied material changed its source.");
    TemporaryTree tree;
    std::string error;
    require(library.saveMaterials(tree.root, error), error);
    AssetLibrary restored;
    require(restored.loadMaterials(tree.root, error), error);
    require(restored.materials.size() == 2 && restored.findMaterial(source.id) && restored.findMaterial(id) &&
        state(*restored.findMaterial(source.id)) == state(source) &&
        state(*restored.findMaterial(id)) == state(*copy), "Saved independent render states did not reload.");
}
} // namespace

int main() {
    try {
        roundTrips();
        compatibility();
        rejectedInputsPreserveState();
        cloneAndPersist();
        std::cout << "PASS: material depth/cull serialization, compatibility, failure recovery and clones ("
            << checks << " checks).\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
