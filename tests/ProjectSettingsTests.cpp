#include <RestArtEngine/Project/Project.h>
#include <RestArtEngine/Editor/ProjectSettingsRules.h>
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
        require(project.rendering.backendPreference == renderer::MainRenderBackend::D3D11Dxr &&
            project.rendering.allowFallbackToD3D11,
            "Projects without a backend choice retain the D3D11 + DXR mode and default fallback policy.");
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

void backendSelectionRoundTrip() {
    Fixture fixture;
    std::string error;
    auto value = fixture.original;
    value["rendering"] = {
        {"allowFallbackToD3D11", false},
        {"hardwareRayTracing", true},
        {"globalIllumination", {{"method", "pathTracing"}, {"extension", {{"retained", 29}}}}},
        {"customRenderer", {{"retained", true}}}
    };
    fixture.write(value);
    ProjectPaths loaded;
    require(loadProject(fixture.manifest, loaded, error), error);
    require(loaded.rendering.backendPreference == renderer::MainRenderBackend::D3D11Dxr &&
        !loaded.rendering.allowFallbackToD3D11 && loaded.rendering.hardwareRayTracing &&
        loaded.rendering.globalIllumination.method == renderer::GlobalIlluminationMethod::PathTracing,
        "A missing backend choice retains hybrid requests even when the fallback field is present.");

    struct Choice { renderer::MainRenderBackend backend; const char* key; };
    for (const auto& choice : {
            Choice{renderer::MainRenderBackend::D3D11Dxr, "d3d11Dxr"},
            Choice{renderer::MainRenderBackend::Auto, "auto"},
            Choice{renderer::MainRenderBackend::D3D11, "d3d11"},
            Choice{renderer::MainRenderBackend::D3D12, "d3d12"}}) {
        for (bool fallback : {false, true}) {
            auto requested = loaded.rendering;
            requested.backendPreference = choice.backend;
            requested.allowFallbackToD3D11 = fallback;
            requested.hardwareRayTracingReflections = false;
            requested.hardwareRayTracingShadows = true;
            requested.globalIllumination.mode = renderer::GlobalIlluminationMode::Realtime;
            requested.globalIllumination.targetSamples = 123;
            require(saveProjectRenderingSettings(fixture.manifest, requested, error), error);
            const auto saved = Json::parse(bytes(fixture.manifest));
            const auto& rendering = saved.at("rendering");
            require(rendering.at("backendPreference") == choice.key &&
                rendering.at("allowFallbackToD3D11") == fallback,
                "Backend selection and fallback policy use their stable schema values.");
            require(loadProject(fixture.manifest, loaded, error) && loaded.rendering == requested,
                "All backend requests round-trip without capabilities rewriting RT or GI intent.");
            require(saved.at("customExtension") == fixture.original.at("customExtension") &&
                rendering.at("customRenderer") == value.at("rendering").at("customRenderer") &&
                rendering.at("globalIllumination").at("extension") ==
                    value.at("rendering").at("globalIllumination").at("extension"),
                "Saving a backend choice preserves unknown project, rendering and nested GI fields.");
            require(fixture.temporaryCount() == 0, "Backend saves leave no temporary manifest.");
        }
    }

    value["rendering"]["backendPreference"] = "legacyHybrid";
    fixture.write(value);
    require(loadProject(fixture.manifest, loaded, error) &&
        loaded.rendering.backendPreference == renderer::MainRenderBackend::D3D11Dxr,
        "The initial legacyHybrid spelling still loads as the explicit D3D11 + DXR mode.");
    const auto compatibilityRequest = loaded.rendering;
    require(saveProjectRenderingSettings(fixture.manifest, compatibilityRequest, error), error);
    require(Json::parse(bytes(fixture.manifest)).at("rendering").at("backendPreference") == "d3d11Dxr",
        "Saving a compatibility spelling normalizes it to the stable D3D11 + DXR schema value.");
    require(loadProject(fixture.manifest, loaded, error) && loaded.rendering == compatibilityRequest,
        "Normalizing the compatibility spelling preserves fallback, RT and GI requests.");
    require(fixture.temporaryCount() == 0, "Compatibility saves leave no temporary manifest.");
}

void backendSelectionValidation() {
    Fixture fixture;
    std::string error;
    const auto reject = [&](const char* field, const Json& invalid) {
        auto value = fixture.original;
        value["rendering"] = {{field, invalid}};
        fixture.write(value);
        const auto before = bytes(fixture.manifest);
        ProjectPaths retained;
        retained.name = "Retain backend project";
        retained.rendering.backendPreference = renderer::MainRenderBackend::D3D12;
        retained.rendering.allowFallbackToD3D11 = false;
        retained.rendering.hardwareRayTracing = true;
        retained.rendering.globalIllumination.method = renderer::GlobalIlluminationMethod::PathTracing;
        const auto priorSettings = retained.rendering;
        require(!loadProject(fixture.manifest, retained, error),
            "Malformed backend settings reject project loading.");
        require(error.find(field) != std::string::npos && retained.name == "Retain backend project" &&
            retained.rendering == priorSettings,
            "Backend parse errors identify the field and preserve previously loaded settings.");
        require(!saveProjectRenderingSettings(fixture.manifest, RenderingSettings{}, error),
            "Saving cannot silently replace malformed existing backend settings.");
        require(bytes(fixture.manifest) == before && fixture.temporaryCount() == 0,
            "Rejected backend metadata remains byte-for-byte intact without temporary files.");
    };
    for (const auto& invalid : {Json(nullptr), Json(false), Json(true), Json(11), Json::array(),
            Json::object(), Json(""), Json("Auto"), Json("dx11"), Json("D3D12"), Json("D3D11Dxr"),
            Json("legacy"), Json("unknown")})
        reject("backendPreference", invalid);
    for (const auto& invalid : {Json(nullptr), Json(0), Json(1), Json("false"), Json::array(), Json::object()})
        reject("allowFallbackToD3D11", invalid);

    fixture.write(fixture.original);
    const auto before = bytes(fixture.manifest);
    for (const int invalid : {-1, 99}) {
        RenderingSettings requested;
        requested.backendPreference = static_cast<renderer::MainRenderBackend>(invalid);
        require(!saveProjectRenderingSettings(fixture.manifest, requested, error) &&
            error.find("backendPreference") != std::string::npos,
            "Unknown in-memory backend enum values cannot be saved.");
        require(bytes(fixture.manifest) == before && fixture.temporaryCount() == 0,
            "Rejected in-memory backend values cannot damage the project.");
    }
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
    require(loaded.rendering.globalIllumination.denoiser == renderer::GlobalIlluminationDenoiser::Spatial &&
        loaded.rendering.globalIllumination.motionVectors == renderer::GlobalIlluminationMotionVectors::Automatic &&
        loaded.rendering.globalIllumination.denoiseIterations == 2 &&
        loaded.rendering.globalIllumination.screenSpaceSteps == 8 &&
        loaded.rendering.globalIllumination.screenSpaceRadius == 2.0f &&
        loaded.rendering.globalIllumination.screenSpaceThickness == 0.2f &&
        loaded.rendering.globalIllumination.probeGridX == 16 && loaded.rendering.globalIllumination.probeGridY == 8 &&
        loaded.rendering.globalIllumination.probeGridZ == 16 && loaded.rendering.globalIllumination.probeSpacing == 1.0f &&
        loaded.rendering.globalIllumination.probeHysteresis == 0.95f &&
        loaded.rendering.globalIllumination.probeAutoFit &&
        loaded.rendering.globalIllumination.probeRayCount == 16 &&
        loaded.rendering.globalIllumination.sampling == renderer::GlobalIlluminationSampling::Uniform &&
        loaded.rendering.globalIllumination.restirCandidates == 4 &&
        loaded.rendering.globalIllumination.restirDirect && loaded.rendering.globalIllumination.restirIndirect &&
        loaded.rendering.globalIllumination.restirSpatialReuse,
        "Missing reconstruction fields keep the legacy spatial resolve defaults.");

    value["rendering"]["globalIllumination"]["method"] = "pathTracing";
    fixture.write(value);
    require(loadProject(fixture.manifest, loaded, error), error);
    require(loaded.rendering.globalIllumination.mode == renderer::GlobalIlluminationMode::Progressive &&
        loaded.rendering.globalIllumination.indirectDiffuse && loaded.rendering.globalIllumination.indirectSpecular &&
        loaded.rendering.globalIllumination.indirectDiffuseIntensity == 1.0f,
        "Older path-tracing projects default to progressive mode and both indirect contributions.");

    RenderingSettings requested;
    requested.hardwareRayTracing = true;
    requested.globalIllumination.method = renderer::GlobalIlluminationMethod::PathTracing;
    requested.globalIllumination.mode = renderer::GlobalIlluminationMode::Realtime;
    requested.globalIllumination.indirectDiffuse = false;
    requested.globalIllumination.indirectSpecular = true;
    requested.globalIllumination.indirectDiffuseIntensity = 2.75f;
    requested.globalIllumination.samplesPerFrame = 4;
    requested.globalIllumination.targetSamples = 512;
    requested.globalIllumination.maxBounces = 6;
    requested.globalIllumination.denoiser = renderer::GlobalIlluminationDenoiser::TemporalSpatial;
    requested.globalIllumination.motionVectors = renderer::GlobalIlluminationMotionVectors::PerObject;
    requested.globalIllumination.denoiseIterations = 3;
    require(saveProjectRenderingSettings(fixture.manifest, requested, error), error);
    require(loadProject(fixture.manifest, loaded, error), error);
    require(loaded.rendering == requested, "GI algorithm and quality settings survive project reload.");
    const auto saved = Json::parse(bytes(fixture.manifest));
    const auto& gi = saved.at("rendering").at("globalIllumination");
    require(gi.at("method") == "pathTracing" && gi.at("mode") == "realtime" &&
        gi.at("indirectDiffuse") == false && gi.at("indirectSpecular") == true && gi.at("samplesPerFrame") == 4 &&
        gi.at("targetSamples") == 512 && gi.at("maxBounces") == 6 && gi.at("indirectDiffuseIntensity") == 2.75f &&
        gi.at("denoiser") == "temporalSpatial" && gi.at("motionVectors") == "perObject" &&
        gi.at("denoiseIterations") == 3,
        "GI serializes independent algorithm, update mode, contributions, reconstruction and quality controls.");
    require(gi.at("extension") == value.at("rendering").at("globalIllumination").at("extension") &&
        saved.at("customExtension") == fixture.original.at("customExtension"),
        "Saving GI preserves unknown nested and project fields.");
    requested.globalIllumination.method = renderer::GlobalIlluminationMethod::ScreenSpace;
    requested.globalIllumination.screenSpaceSteps = 12;
    requested.globalIllumination.screenSpaceRadius = 4.5f;
    requested.globalIllumination.screenSpaceThickness = 0.35f;
    require(saveProjectRenderingSettings(fixture.manifest, requested, error), error);
    require(loadProject(fixture.manifest, loaded, error) && loaded.rendering == requested,
        "SSGI method and march settings survive project reload.");
    const auto savedSsgi = Json::parse(bytes(fixture.manifest)).at("rendering").at("globalIllumination");
    require(savedSsgi.at("method") == "screenSpace" && savedSsgi.at("screenSpaceSteps") == 12 &&
        savedSsgi.at("screenSpaceRadius") == 4.5f && savedSsgi.at("screenSpaceThickness") == 0.35f,
        "SSGI serializes its stable method name and march settings.");
    requested.globalIllumination.method = renderer::GlobalIlluminationMethod::ProbeGrid;
    requested.globalIllumination.probeGridX = 20;
    requested.globalIllumination.probeGridY = 10;
    requested.globalIllumination.probeGridZ = 24;
    requested.globalIllumination.probeSpacing = 1.5f;
    requested.globalIllumination.probeHysteresis = 0.8f;
    requested.globalIllumination.probeAutoFit = false;
    requested.globalIllumination.probeRayCount = 24;
    require(saveProjectRenderingSettings(fixture.manifest, requested, error), error);
    require(loadProject(fixture.manifest, loaded, error) && loaded.rendering == requested,
        "DDGI probe grid settings survive project reload.");
    const auto savedDdgi = Json::parse(bytes(fixture.manifest)).at("rendering").at("globalIllumination");
    require(savedDdgi.at("method") == "probeGrid" && savedDdgi.at("probeGridX") == 20 &&
        savedDdgi.at("probeGridY") == 10 && savedDdgi.at("probeGridZ") == 24 &&
        savedDdgi.at("probeSpacing") == 1.5f && savedDdgi.at("probeHysteresis") == 0.8f &&
        savedDdgi.at("probeAutoFit") == false && savedDdgi.at("probeRayCount") == 24,
        "DDGI serializes its probe grid and history controls.");
    requested.globalIllumination.sampling = renderer::GlobalIlluminationSampling::Restir;
    requested.globalIllumination.restirCandidates = 7;
    requested.globalIllumination.restirDirect = false;
    requested.globalIllumination.restirIndirect = true;
    requested.globalIllumination.restirSpatialReuse = false;
    require(saveProjectRenderingSettings(fixture.manifest, requested, error), error);
    require(loadProject(fixture.manifest, loaded, error) && loaded.rendering == requested,
        "ReSTIR sampling settings survive project reload.");
    const auto savedRestir = Json::parse(bytes(fixture.manifest)).at("rendering").at("globalIllumination");
    require(savedRestir.at("sampling") == "restir" && savedRestir.at("restirCandidates") == 7 &&
        savedRestir.at("restirDirect") == false && savedRestir.at("restirIndirect") == true &&
        savedRestir.at("restirSpatialReuse") == false,
        "ReSTIR serializes its sampling layer controls.");
    requested.globalIllumination.method = renderer::GlobalIlluminationMethod::PathTracing;

    requested.hardwareRayTracing = false;
    requested.globalIllumination.mode = renderer::GlobalIlluminationMode::Progressive;
    requested.globalIllumination.indirectDiffuse = true;
    requested.globalIllumination.indirectSpecular = false;
    requested.globalIllumination.indirectDiffuseIntensity = renderer::kGiMaxIndirectDiffuseIntensity;
    requested.globalIllumination.samplesPerFrame = renderer::kGiMaxSamplesPerFrame;
    requested.globalIllumination.targetSamples = renderer::kGiMaxTargetSamples;
    requested.globalIllumination.maxBounces = renderer::kGiMaxBounces;
    require(saveProjectRenderingSettings(fixture.manifest, requested, error), error);
    require(loadProject(fixture.manifest, loaded, error) && loaded.rendering == requested,
        "Turning hardware off retains the requested GI algorithm and upper quality bounds.");
    requested.globalIllumination.method = renderer::GlobalIlluminationMethod::Disabled;
    requested.globalIllumination.mode = renderer::GlobalIlluminationMode::Realtime;
    requested.globalIllumination.indirectDiffuse = false;
    requested.globalIllumination.indirectDiffuseIntensity = 0.0f;
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
    for (const auto& invalid : {Json(nullptr), Json(false), Json(1), Json("unknown"), Json("Realtime"), Json::array()})
        reject(Json{{"mode", invalid}}, "mode");
    for (const auto& invalid : {Json(nullptr), Json(false), Json(1), Json("unknown"), Json("Spatial"), Json::array()})
        reject(Json{{"denoiser", invalid}}, "denoiser");
    for (const auto& invalid : {Json(nullptr), Json(false), Json(1), Json("unknown"), Json("PerObject"), Json::array()})
        reject(Json{{"motionVectors", invalid}}, "motionVectors");
    for (const auto& invalid : {Json(nullptr), Json(false), Json(1), Json("unknown"), Json("Restir"), Json::array()})
        reject(Json{{"sampling", invalid}}, "sampling");
    for (const char* restirBool : {"restirDirect", "restirIndirect", "restirSpatialReuse"})
        for (const auto& invalid : {Json(nullptr), Json(0), Json(1), Json("true"), Json::array(), Json::object()})
            reject(Json{{restirBool, invalid}}, restirBool);
    for (const auto& invalid : {Json(nullptr), Json(0), Json(1), Json("true"), Json::array(), Json::object()})
        reject(Json{{"probeAutoFit", invalid}}, "probeAutoFit");
    for (const auto& invalid : {Json(0), Json(-1), Json(33), Json("16"), Json(1.5), Json(nullptr)})
        reject(Json{{"probeRayCount", invalid}}, "probeRayCount");
    for (const char* contribution : {"indirectDiffuse", "indirectSpecular"})
        for (const auto& invalid : {Json(nullptr), Json(0), Json(1), Json("true"), Json::array(), Json::object()})
            reject(Json{{contribution, invalid}}, contribution);
    for (const auto& invalid : {Json(nullptr), Json(false), Json(true), Json("2"), Json(-0.01),
            Json(8.01), Json(1e100), Json::array(), Json::object()})
        reject(Json{{"indirectDiffuseIntensity", invalid}}, "indirectDiffuseIntensity");
    for (const auto& invalid : {Json(nullptr), Json(false), Json("2"), Json(0.0), Json(-1.0),
            Json(100.1), Json::array(), Json::object()})
        reject(Json{{"screenSpaceRadius", invalid}}, "screenSpaceRadius");
    for (const auto& invalid : {Json(nullptr), Json(false), Json("0.2"), Json(0.0), Json(-1.0),
            Json(10.1), Json::array(), Json::object()})
        reject(Json{{"screenSpaceThickness", invalid}}, "screenSpaceThickness");
    for (const auto& invalid : {Json(nullptr), Json(false), Json("1"), Json(0.0), Json(-1.0),
            Json(20.1), Json::array(), Json::object()})
        reject(Json{{"probeSpacing", invalid}}, "probeSpacing");
    for (const auto& invalid : {Json(nullptr), Json(false), Json("0.9"), Json(-0.1), Json(1.1),
            Json::array(), Json::object()})
        reject(Json{{"probeHysteresis", invalid}}, "probeHysteresis");

    struct Bound { const char* key; std::uint32_t maximum; };
    for (const auto& bound : {Bound{"samplesPerFrame", renderer::kGiMaxSamplesPerFrame},
            Bound{"targetSamples", renderer::kGiMaxTargetSamples}, Bound{"maxBounces", renderer::kGiMaxBounces},
            Bound{"denoiseIterations", renderer::kGiMaxDenoiseIterations},
            Bound{"screenSpaceSteps", renderer::kGiMaxScreenSpaceSteps},
            Bound{"probeGridX", renderer::kGiMaxProbeGridAxis},
            Bound{"probeGridY", renderer::kGiMaxProbeGridAxis},
            Bound{"probeGridZ", renderer::kGiMaxProbeGridAxis},
            Bound{"restirCandidates", renderer::kGiMaxRestirCandidates}}) {
        for (const auto& invalid : {Json(nullptr), Json(false), Json("4"), Json(1.0), Json(1.5),
                Json(0), Json(-1), Json(bound.maximum + 1), Json(std::numeric_limits<std::uint64_t>::max()),
                Json(std::numeric_limits<std::int64_t>::min()), Json::object()})
            reject(Json{{bound.key, invalid}}, bound.key);
    }

    fixture.write(fixture.original);
    const auto before = bytes(fixture.manifest);
    int invalidCase = 0;
    const auto invalidRequest = [&](const RenderingSettings& settings) {
        ++invalidCase;
        require(!saveProjectRenderingSettings(fixture.manifest, settings, error),
            "Invalid runtime GI settings cannot be saved (case " + std::to_string(invalidCase) + "): " + error);
        require(bytes(fixture.manifest) == before && fixture.temporaryCount() == 0,
            "Invalid GI requests cannot damage the project.");
    };
    RenderingSettings invalid;
    for (const float intensity : {-1.0f, renderer::kGiMaxIndirectDiffuseIntensity + 1.0f,
            std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        invalid.globalIllumination.indirectDiffuseIntensity = intensity;
        invalidRequest(invalid);
    }
    invalid = {};
    invalid.globalIllumination.method = static_cast<renderer::GlobalIlluminationMethod>(99);
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.mode = static_cast<renderer::GlobalIlluminationMode>(99);
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.denoiser = static_cast<renderer::GlobalIlluminationDenoiser>(99);
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.motionVectors = static_cast<renderer::GlobalIlluminationMotionVectors>(99);
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.denoiseIterations = 0;
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.denoiseIterations = renderer::kGiMaxDenoiseIterations + 1;
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
    invalid = {};
    invalid.globalIllumination.screenSpaceSteps = 0;
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.screenSpaceSteps = renderer::kGiMaxScreenSpaceSteps + 1;
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.screenSpaceRadius = 0.0f;
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.screenSpaceRadius = std::numeric_limits<float>::infinity();
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.screenSpaceThickness = std::numeric_limits<float>::quiet_NaN();
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.screenSpaceThickness = 11.0f;
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.probeGridX = 1;
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.probeGridY = renderer::kGiMaxProbeGridAxis + 1;
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.probeSpacing = 0.0f;
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.probeHysteresis = 1.1f;
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.sampling = static_cast<renderer::GlobalIlluminationSampling>(99);
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.restirCandidates = 0;
    invalidRequest(invalid);
    invalid = {};
    invalid.globalIllumination.restirCandidates = renderer::kGiMaxRestirCandidates + 1;
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

void editingDependenciesAndRestart() {
    using namespace rae::editor;
    using namespace renderer;
    RenderingSettings draft;
    draft.hardwareRayTracing = true;
    draft.globalIllumination.method = GlobalIlluminationMethod::PathTracing;
    draft.globalIllumination.targetSamples = 901;
    draft.hardwareRayTracing = false;
    applyProjectRenderingEdit(draft, ProjectRenderingEdit::HardwareRayTracing);
    require(!draft.hardwareRayTracingReflections && !draft.hardwareRayTracingShadows &&
        draft.globalIllumination.method == GlobalIlluminationMethod::Disabled,
        "An explicit hardware-off edit clears dependent effects and GI.");
    require(draft.globalIllumination.targetSamples == 901 && draft.globalIllumination.indirectDiffuse,
        "Clearing the parent preserves GI quality and contribution tuning.");
    draft.hardwareRayTracing = true;
    applyProjectRenderingEdit(draft, ProjectRenderingEdit::HardwareRayTracing);
    require(draft.hardwareRayTracingReflections && draft.hardwareRayTracingShadows,
        "Hardware-on selects its default effects.");
    draft.hardwareRayTracing = false;
    draft.globalIllumination.method = GlobalIlluminationMethod::PathTracing;
    applyProjectRenderingEdit(draft, ProjectRenderingEdit::GiMethod);
    require(draft.hardwareRayTracing && !draft.hardwareRayTracingReflections && draft.hardwareRayTracingShadows,
        "Path tracing enables its hardware dependency and clears competing reflections.");
    draft.globalIllumination.mode = GlobalIlluminationMode::Realtime;
    applyProjectRenderingEdit(draft, ProjectRenderingEdit::GiMode);
    require(!draft.hardwareRayTracingReflections && !draft.hardwareRayTracingShadows,
        "Realtime GI also clears the standalone ray-shadow effect.");
    draft.globalIllumination.method = GlobalIlluminationMethod::Disabled;
    applyProjectRenderingEdit(draft, ProjectRenderingEdit::GiMethod);
    require(!draft.hardwareRayTracingReflections && !draft.hardwareRayTracingShadows,
        "Leaving GI does not silently re-enable effects the user did not select.");
    for (auto backend : {MainRenderBackend::Auto, MainRenderBackend::D3D11}) {
        draft.backendPreference = backend;
        draft.hardwareRayTracing = draft.hardwareRayTracingReflections = draft.hardwareRayTracingShadows = true;
        draft.globalIllumination.method = GlobalIlluminationMethod::PathTracing;
        applyProjectRenderingEdit(draft, ProjectRenderingEdit::Backend);
        require(!draft.hardwareRayTracing && !draft.hardwareRayTracingReflections && !draft.hardwareRayTracingShadows &&
            draft.globalIllumination.method == GlobalIlluminationMethod::Disabled,
            "An explicit incompatible backend choice clears hardware feature requests.");
        draft.globalIllumination.method = GlobalIlluminationMethod::ScreenSpace;
        applyProjectRenderingEdit(draft, ProjectRenderingEdit::Backend);
        require(!draft.hardwareRayTracing && draft.globalIllumination.method == GlobalIlluminationMethod::ScreenSpace,
            "SSGI survives an incompatible backend because it is a raster algorithm.");
        draft.globalIllumination.method = GlobalIlluminationMethod::ProbeGrid;
        applyProjectRenderingEdit(draft, ProjectRenderingEdit::Backend);
        require(!draft.hardwareRayTracing && draft.globalIllumination.method == GlobalIlluminationMethod::ProbeGrid,
            "DDGI survives an incompatible backend because it is a raster algorithm.");
        require(!projectBackendAllowsHardware(backend), "Strict D3D11 variants do not expose hardware/GI editing.");
    }
    require(projectBackendAllowsHardware(MainRenderBackend::D3D11Dxr), "The hybrid backend allows feature requests.");
    require(projectBackendAllowsHardware(MainRenderBackend::D3D12),
        "The native D3D12 backend exposes native DXR hardware/GI requests.");
    draft.backendPreference = MainRenderBackend::D3D12;
    draft.hardwareRayTracing = draft.hardwareRayTracingReflections = draft.hardwareRayTracingShadows = true;
    draft.globalIllumination.method = GlobalIlluminationMethod::PathTracing;
    applyProjectRenderingEdit(draft, ProjectRenderingEdit::Backend);
    require(draft.hardwareRayTracing && draft.globalIllumination.method == GlobalIlluminationMethod::PathTracing,
        "Selecting native D3D12 preserves compatible hardware requests for native DXR.");

    RenderingSettings saved;
    const auto launched = projectBackendRequest(saved);
    draft = saved;
    draft.hardwareRayTracing = true;
    draft.globalIllumination.method = GlobalIlluminationMethod::PathTracing;
    require(projectRestartNotice(draft, saved, launched) == ProjectRestartNotice::None,
        "Live lighting edits do not claim a restart requirement.");
    draft.backendPreference = MainRenderBackend::D3D12;
    require(projectRestartNotice(draft, saved, launched) == ProjectRestartNotice::SaveAndRestart,
        "Unsaved startup edits request both saving and restarting.");
    saved = draft;
    require(projectRestartNotice(draft, saved, launched) == ProjectRestartNotice::Restart,
        "Saving does not hide a pending startup change.");
    draft.backendPreference = launched.preference;
    require(projectRestartNotice(draft, saved, launched) == ProjectRestartNotice::SaveToCancelRestart,
        "An unsaved revert explains that the persisted startup change still needs cancellation.");
    saved = draft;
    draft.allowFallbackToD3D11 = !draft.allowFallbackToD3D11;
    require(projectRestartNotice(draft, saved, launched) == ProjectRestartNotice::None,
        "An irrelevant fallback preference does not request restarting a non-native12 session.");
    saved.backendPreference = draft.backendPreference = MainRenderBackend::D3D12;
    auto nativeLaunch = projectBackendRequest(saved);
    draft.allowFallbackToD3D11 = !saved.allowFallbackToD3D11;
    require(projectRestartNotice(draft, saved, nativeLaunch) == ProjectRestartNotice::SaveAndRestart,
        "Native12 fallback policy is a real startup setting.");
    const auto preserved = draft;
    (void)projectRestartNotice(draft, saved, nativeLaunch);
    (void)projectBackendAllowsHardware(draft.backendPreference);
    require(draft == preserved, "Reading capability/restart state does not normalize or erase stored requests.");
}

int main() {
    try {
        defaultsAndRoundTrip();
        validationAndTransactions();
        backendSelectionRoundTrip();
        backendSelectionValidation();
        globalIlluminationRoundTrip();
        globalIlluminationValidation();
        replacementFailure();
        editingDependenciesAndRestart();
        std::cout << "ProjectSettingsTests: " << checks << " checks passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "ProjectSettingsTests failed: " << exception.what() << '\n';
        return 1;
    }
}
