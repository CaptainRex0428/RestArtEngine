#include <RestArtEngine/Project/Project.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {
namespace fs = std::filesystem;
namespace renderer = rae::renderer;
using Json = nlohmann::json;
using namespace rae::project;
int checks = 0;

void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

void writeText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    file.close();
    if (!file) throw std::runtime_error("Cannot write isolated project fixture.");
}

std::string bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read isolated project fixture.");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

struct Fixture {
    fs::path root = fs::temp_directory_path() / ("rae_project_settings_test_" +
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    fs::path project = root / "Project";
    fs::path manifest = project / "Settings.raproject";
    Json original = {
        {"format", "RAEngine.Project"}, {"version", 3}, {"name", "Settings fixture"},
        {"engineRoot", "../Engine"}, {"contentRoot", "Content"},
        {"shaderRoot", "Shaders"}, {"defaultScene", "Content/Scenes/Default.rascene"},
        {"customExtension", {{"preserved", "unchanged"}, {"number", 42}}}
    };
    Fixture() {
        if (!fs::create_directory(root)) throw std::runtime_error("Cannot create isolated fixture directory.");
        writeText(root / "Engine/Shaders/Private/Scene.hlsl", "// fixture only\n");
        writeText(project / "Content/Scenes/Default.rascene", "{}\n");
        fs::create_directories(project / "Shaders");
        write(original);
    }
    ~Fixture() {
        std::error_code ignored;
        if (root.parent_path() == fs::temp_directory_path() &&
            root.filename().string().starts_with("rae_project_settings_test_")) fs::remove_all(root, ignored);
    }
    void write(const Json& value) const { writeText(manifest, value.dump(2) + '\n'); }
    std::size_t temporaryCount() const {
        std::size_t count = 0;
        for (const auto& item : fs::directory_iterator(project))
            if (item.path().filename().string().find(".tmp-settings-") != std::string::npos) ++count;
        return count;
    }
};

void defaultsAndRoundTrip() {
    Fixture fixture;
    std::string error;
    for (int version : {1, 2, 3}) {
        auto value = fixture.original;
        value["version"] = version;
        fixture.write(value);
        ProjectPaths project;
        require(loadProject(fixture.manifest, project, error), "Legacy project loads: " + error);
        require(project.rendering == RenderingSettings{}, "Missing rendering fields retain compatible defaults.");
    }
    auto value = fixture.original;
    value["rendering"] = {{"customRenderer", {{"retained", true}}}};
    fixture.write(value);
    RenderingSettings requested;
    requested.hardwareRayTracing = true;
    requested.hardwareRayTracingReflections = false;
    requested.hardwareRayTracingShadows = true;
    require(saveProjectRenderingSettings(fixture.project, requested, error), "Save requested hardware features: " + error);
    const auto saved = Json::parse(bytes(fixture.manifest));
    require(saved.at("customExtension") == value.at("customExtension"), "Unknown project fields survive save.");
    require(saved.at("engineRoot") == value.at("engineRoot") && saved.at("version") == 3,
        "Saving rendering does not rebind or upgrade the project.");
    require(saved.at("rendering").at("customRenderer") == value.at("rendering").at("customRenderer"),
        "Unknown rendering settings survive save.");
    ProjectPaths reloaded;
    require(loadProject(fixture.manifest, reloaded, error), "Reload saved settings: " + error);
    require(reloaded.rendering == requested, "Requests round-trip independently of this machine's GPU.");
    require(fixture.temporaryCount() == 0, "Successful save does not leave temporary manifests.");
    requested.hardwareRayTracing = false;
    requested.hardwareRayTracingReflections = true;
    requested.hardwareRayTracingShadows = false;
    require(saveProjectRenderingSettings(fixture.manifest, requested, error), "Save disabled features: " + error);
    require(loadProject(fixture.manifest, reloaded, error) && reloaded.rendering == requested,
        "Disabling the master keeps separate effect preferences.");
}

void validationAndTransactions() {
    Fixture fixture;
    std::string error;
    for (const auto& invalid : {Json(nullptr), Json(false), Json(5), Json("enabled"), Json::array()}) {
        auto value = fixture.original;
        value["rendering"] = invalid;
        fixture.write(value);
        ProjectPaths retained;
        retained.name = "Keep previous project";
        retained.rendering.hardwareRayTracing = true;
        require(!loadProject(fixture.manifest, retained, error), "Rendering requires an object.");
        require(retained.name == "Keep previous project" && retained.rendering.hardwareRayTracing,
            "Failed parsing preserves the previously loaded project.");
        const auto before = bytes(fixture.manifest);
        require(!saveProjectRenderingSettings(fixture.manifest, RenderingSettings{}, error),
            "Do not silently overwrite malformed rendering metadata.");
        require(bytes(fixture.manifest) == before && fixture.temporaryCount() == 0,
            "Malformed metadata is retained without temporary files.");
    }
    for (const char* key : {"hardwareRayTracing", "hardwareRayTracingReflections", "hardwareRayTracingShadows"}) {
        for (const auto& invalid : {Json(nullptr), Json(0), Json("true"), Json::object()}) {
            auto value = fixture.original;
            value["rendering"] = {{key, invalid}};
            fixture.write(value);
            ProjectPaths retained;
            retained.name = "Unchanged";
            require(!loadProject(fixture.manifest, retained, error), std::string(key) + " requires a boolean.");
            require(error.find(key) != std::string::npos && retained.name == "Unchanged",
                "Invalid field identifies the key and preserves the output.");
        }
    }
    auto value = fixture.original;
    value["rendering"] = {{"hardwareRayTracing", true}};
    fixture.write(value);
    ProjectPaths partial;
    require(loadProject(fixture.manifest, partial, error), "Partial settings object is accepted.");
    require(partial.rendering.hardwareRayTracing && partial.rendering.hardwareRayTracingReflections &&
        partial.rendering.hardwareRayTracingShadows, "Missing per-effect values use their defaults.");

    writeText(fixture.manifest, "{ broken JSON");
    const auto broken = bytes(fixture.manifest);
    require(!saveProjectRenderingSettings(fixture.manifest, RenderingSettings{}, error), "Malformed project rejects save.");
    require(bytes(fixture.manifest) == broken && fixture.temporaryCount() == 0, "Broken project remains byte-for-byte intact.");
    value = fixture.original;
    value["version"] = 99;
    fixture.write(value);
    const auto unsupported = bytes(fixture.manifest);
    require(!saveProjectRenderingSettings(fixture.manifest, RenderingSettings{}, error), "Unsupported version rejects save.");
    require(bytes(fixture.manifest) == unsupported, "Unknown project versions are not downgraded.");
}

void globalIlluminationRoundTrip() {
    Fixture fixture;
    std::string error;
    auto value = fixture.original;
    value["rendering"] = {{"globalIllumination", {{"extension", {{"preserved", 81}}}}}};
    fixture.write(value);
    ProjectPaths loaded;
    require(loadProject(fixture.manifest, loaded, error), error);
    require(loaded.rendering.globalIllumination == renderer::GlobalIlluminationSettings{},
        "An empty GI settings object preserves compatible defaults.");

    RenderingSettings requested;
    requested.hardwareRayTracing = true;
    requested.globalIllumination.method = renderer::GlobalIlluminationMethod::PathTracing;
    requested.globalIllumination.samplesPerFrame = 4;
    requested.globalIllumination.targetSamples = 512;
    requested.globalIllumination.maxBounces = 6;
    require(saveProjectRenderingSettings(fixture.manifest, requested, error), error);
    require(loadProject(fixture.manifest, loaded, error), error);
    require(loaded.rendering == requested, "GI algorithm and quality settings survive project reload.");
    const auto saved = Json::parse(bytes(fixture.manifest));
    const auto& gi = saved.at("rendering").at("globalIllumination");
    require(gi.at("method") == "pathTracing" && gi.at("samplesPerFrame") == 4 &&
        gi.at("targetSamples") == 512 && gi.at("maxBounces") == 6,
        "GI serializes stable method names and integer quality controls.");
    require(gi.at("extension") == value.at("rendering").at("globalIllumination").at("extension") &&
        saved.at("customExtension") == fixture.original.at("customExtension"),
        "Saving GI preserves unknown nested and project fields.");

    requested.hardwareRayTracing = false;
    requested.globalIllumination.samplesPerFrame = renderer::kGiMaxSamplesPerFrame;
    requested.globalIllumination.targetSamples = renderer::kGiMaxTargetSamples;
    requested.globalIllumination.maxBounces = renderer::kGiMaxBounces;
    require(saveProjectRenderingSettings(fixture.manifest, requested, error), error);
    require(loadProject(fixture.manifest, loaded, error) && loaded.rendering == requested,
        "Turning hardware off retains the requested GI algorithm and upper quality bounds.");
    requested.globalIllumination.method = renderer::GlobalIlluminationMethod::Disabled;
    requested.globalIllumination.samplesPerFrame = requested.globalIllumination.targetSamples =
        requested.globalIllumination.maxBounces = 1;
    require(saveProjectRenderingSettings(fixture.manifest, requested, error), error);
    require(loadProject(fixture.manifest, loaded, error) && loaded.rendering == requested,
        "Disabled GI and lower integer bounds round-trip.");
    require(Json::parse(bytes(fixture.manifest)).at("rendering").at("globalIllumination").at("method") == "disabled",
        "Disabled GI uses its stable method name.");
    require(fixture.temporaryCount() == 0, "GI saves leave no temporary manifests.");
}

void globalIlluminationValidation() {
    Fixture fixture;
    std::string error;
    const auto reject = [&](const Json& gi, const char* field) {
        auto value = fixture.original;
        value["rendering"] = {{"globalIllumination", gi}};
        fixture.write(value);
        const auto before = bytes(fixture.manifest);
        ProjectPaths previous;
        previous.name = "Retain project";
        previous.rendering.hardwareRayTracing = true;
        previous.rendering.globalIllumination.method = renderer::GlobalIlluminationMethod::PathTracing;
        previous.rendering.globalIllumination.targetSamples = 64;
        const auto retained = previous.rendering;
        require(!loadProject(fixture.manifest, previous, error), "Malformed GI settings reject project loading.");
        require(previous.name == "Retain project" && previous.rendering == retained,
            "GI parse failure leaves previous settings unchanged.");
        require(error.find(field) != std::string::npos, "GI diagnostic identifies the invalid field.");
        require(!saveProjectRenderingSettings(fixture.manifest, RenderingSettings{}, error),
            "Saving rejects malformed existing GI metadata.");
        require(bytes(fixture.manifest) == before && fixture.temporaryCount() == 0,
            "Rejected GI metadata remains byte-for-byte intact without temporary files.");
    };
    for (const auto& invalid : {Json(nullptr), Json(true), Json(1), Json("pathTracing"), Json::array()})
        reject(invalid, "globalIllumination");
    for (const auto& invalid : {Json(nullptr), Json(false), Json(1), Json("unknown"), Json("PathTracing"), Json::array()})
        reject(Json{{"method", invalid}}, "method");

    struct Bound { const char* key; std::uint32_t maximum; };
    for (const auto& bound : {Bound{"samplesPerFrame", renderer::kGiMaxSamplesPerFrame},
            Bound{"targetSamples", renderer::kGiMaxTargetSamples}, Bound{"maxBounces", renderer::kGiMaxBounces}}) {
        for (const auto& invalid : {Json(nullptr), Json(false), Json("4"), Json(1.0), Json(1.5),
                Json(0), Json(-1), Json(bound.maximum + 1), Json(std::numeric_limits<std::uint64_t>::max()),
                Json(std::numeric_limits<std::int64_t>::min()), Json::object()})
            reject(Json{{bound.key, invalid}}, bound.key);
    }

    fixture.write(fixture.original);
    const auto before = bytes(fixture.manifest);
    const auto invalidRequest = [&](const RenderingSettings& settings) {
        require(!saveProjectRenderingSettings(fixture.manifest, settings, error),
            "Invalid runtime GI settings cannot be saved.");
        require(bytes(fixture.manifest) == before && fixture.temporaryCount() == 0,
            "Invalid GI requests cannot damage the project.");
    };
    RenderingSettings invalid;
    invalid.globalIllumination.method = static_cast<renderer::GlobalIlluminationMethod>(99);
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.samplesPerFrame = 0;
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.samplesPerFrame = renderer::kGiMaxSamplesPerFrame + 1;
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.targetSamples = renderer::kGiMaxTargetSamples + 1;
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.maxBounces = renderer::kGiMaxBounces + 1;
    invalidRequest(invalid);
}

void replacementFailure() {
    Fixture fixture;
    const auto before = bytes(fixture.manifest);
    std::string error;
    RenderingSettings requested;
    requested.hardwareRayTracing = true;
#ifdef _WIN32
    // Permit reading but deny deletion/rename, so the write reaches the atomic
    // replacement and fails. The original file must remain intact.
    const auto lock = CreateFileW(fixture.manifest.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(lock != INVALID_HANDLE_VALUE, "Lock isolated project against replacement.");
    const bool saved = saveProjectRenderingSettings(fixture.manifest, requested, error);
    CloseHandle(lock);
    require(!saved && error.find("replace") != std::string::npos, "Atomic replacement failure is reported.");
    require(bytes(fixture.manifest) == before, "Replacement failure never deletes the original manifest.");
    require(fixture.temporaryCount() == 0, "Replacement failure removes only its temporary file.");
    require(saveProjectRenderingSettings(fixture.manifest, requested, error), "Saving can be retried after failure: " + error);
#endif
    require(!saveProjectRenderingSettings(fixture.project / "missing.raproject", requested, error),
        "Missing project is rejected without creating a replacement.");
    require(fixture.temporaryCount() == 0, "Missing project leaves no temporary file.");
}
} // namespace

int main() {
    try {
        defaultsAndRoundTrip();
        validationAndTransactions();
        globalIlluminationRoundTrip();
        globalIlluminationValidation();
        replacementFailure();
        std::cout << "ProjectSettingsTests: " << checks << " checks passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "ProjectSettingsTests failed: " << exception.what() << '\n';
        return 1;
    }
}
