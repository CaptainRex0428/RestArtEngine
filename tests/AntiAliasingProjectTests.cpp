#include <RestArtEngine/Project/Project.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace rae;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

std::string bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "Cannot read isolated fixture.");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    output.close();
    require(bool(output), "Cannot write isolated fixture.");
}

struct Fixture {
    fs::path root = fs::temp_directory_path() / ("rae_aa_settings_" +
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    fs::path manifest = root / "Project/Fixture.raproject";
    Json original{{"format", "RAEngine.Project"}, {"version", 3}, {"name", "AA fixture"},
        {"engineRoot", "../Engine"}, {"contentRoot", "Content"}, {"shaderRoot", "Shaders"},
        {"defaultScene", "Content/Scenes/Default.rascene"}, {"extension", "preserve"}};
    Fixture() {
        require(fs::create_directory(root), "Cannot create isolated fixture.");
        write(root / "Engine/Shaders/Private/Scene.hlsl", "// test fixture\n");
        write(root / "Project/Content/Scenes/Default.rascene", "{}\n");
        fs::create_directories(root / "Project/Shaders");
        store(original);
    }
    ~Fixture() {
        std::error_code ignored;
        if (root.parent_path() == fs::temp_directory_path() && root.filename().string().starts_with("rae_aa_settings_"))
            fs::remove_all(root, ignored);
    }
    void store(const Json& value) const { write(manifest, value.dump(2) + '\n'); }
};

void defaultsAndRoundTrip() {
    Fixture fixture;
    std::string error;
    project::ProjectPaths loaded;
    for (int version : {1, 2, 3}) {
        auto value = fixture.original;
        value["version"] = version;
        fixture.store(value);
        require(project::loadProject(fixture.manifest, loaded, error), error);
        require(loaded.rendering.antiAliasing.method == renderer::AntiAliasingMethod::FXAA &&
            loaded.rendering.antiAliasing.temporalCurrentWeight == .1f,
            "Legacy projects default to FXAA and the standard TAA response.");
    }
    auto value = fixture.original;
    value["rendering"] = {{"extension", 17}, {"antiAliasing", {{"future", "preserve"}}}};
    fixture.store(value);
    for (const auto method : {renderer::AntiAliasingMethod::Disabled, renderer::AntiAliasingMethod::FXAA,
                              renderer::AntiAliasingMethod::TAA}) {
        for (const float weight : {.02f, .1f, 1.0f}) {
            project::RenderingSettings settings;
            settings.antiAliasing.method = method;
            settings.antiAliasing.temporalCurrentWeight = weight;
            require(project::saveProjectRenderingSettings(fixture.manifest, settings, error), error);
            require(project::loadProject(fixture.manifest, loaded, error), error);
            require(loaded.rendering == settings, "AA request and weight must round-trip exactly.");
            const auto saved = Json::parse(bytes(fixture.manifest));
            require(saved.at("extension") == "preserve" && saved.at("rendering").at("extension") == 17 &&
                saved.at("rendering").at("antiAliasing").at("future") == "preserve",
                "Saving AA preserves project, rendering and nested extension fields.");
        }
    }
}

void validationAndTransactions() {
    Fixture fixture;
    std::string error;
    const auto rejectLoad = [&](const Json& aa) {
        auto value = fixture.original;
        value["rendering"] = {{"antiAliasing", aa}};
        fixture.store(value);
        project::ProjectPaths retained;
        retained.name = "Retained";
        retained.rendering.antiAliasing.method = renderer::AntiAliasingMethod::TAA;
        require(!project::loadProject(fixture.manifest, retained, error) && error.find("antiAliasing") != std::string::npos,
            "Invalid AA input must fail with an identified field.");
        require(retained.name == "Retained" && retained.rendering.antiAliasing.method == renderer::AntiAliasingMethod::TAA,
            "Failed loading must retain the previous project.");
        const auto original = bytes(fixture.manifest);
        require(!project::saveProjectRenderingSettings(fixture.manifest, project::RenderingSettings{}, error),
            "Saving must not silently replace malformed AA metadata.");
        require(bytes(fixture.manifest) == original, "Malformed input must remain byte-identical.");
    };
    for (const auto& invalid : {Json(nullptr), Json(false), Json(1), Json("taa"), Json::array()}) rejectLoad(invalid);
    for (const auto& invalid : {Json(nullptr), Json(2), Json(true), Json("TAA"), Json("msaa"), Json::object()})
        rejectLoad(Json{{"method", invalid}});
    for (const auto& invalid : {Json(nullptr), Json(true), Json("0.1"), Json(-.1), Json(.019), Json(1.01), Json::array()})
        rejectLoad(Json{{"temporalCurrentWeight", invalid}});

    fixture.store(fixture.original);
    const auto original = bytes(fixture.manifest);
    for (const float weight : {-.1f, .019f, 1.01f, std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity()}) {
        project::RenderingSettings settings;
        settings.antiAliasing.temporalCurrentWeight = weight;
        require(!project::saveProjectRenderingSettings(fixture.manifest, settings, error),
            "Invalid AA weight must fail before writing.");
        require(bytes(fixture.manifest) == original, "Failed save must preserve the manifest.");
    }
    project::RenderingSettings settings;
    settings.antiAliasing.method = static_cast<renderer::AntiAliasingMethod>(99);
    require(!project::saveProjectRenderingSettings(fixture.manifest, settings, error) && bytes(fixture.manifest) == original,
        "Unknown AA method must not modify the project.");
}
}

int main() {
    try {
        defaultsAndRoundTrip();
        validationAndTransactions();
        std::cout << "PASS: AA defaults, exact round-trip, extension preservation and transactional validation.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
