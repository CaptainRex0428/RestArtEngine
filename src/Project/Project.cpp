#include "RestArtEngine/Project/Project.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <atomic>
#include <chrono>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace rae::project {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

void validateGlobalIllumination(const renderer::GlobalIlluminationSettings& settings) {
    using renderer::GlobalIlluminationMethod;
    using renderer::GlobalIlluminationMode;
    if (settings.method != GlobalIlluminationMethod::Disabled && settings.method != GlobalIlluminationMethod::PathTracing)
        throw std::runtime_error("rendering.globalIllumination.method must be disabled or pathTracing.");
    if (settings.mode != GlobalIlluminationMode::Progressive && settings.mode != GlobalIlluminationMode::Realtime)
        throw std::runtime_error("rendering.globalIllumination.mode must be progressive or realtime.");
    const auto range = [](const char* key, std::uint32_t value, std::uint32_t maximum) {
        if (value < 1 || value > maximum)
            throw std::runtime_error(std::string("rendering.globalIllumination.") + key +
                " must be an integer from 1 to " + std::to_string(maximum) + ".");
    };
    range("samplesPerFrame", settings.samplesPerFrame, renderer::kGiMaxSamplesPerFrame);
    range("targetSamples", settings.targetSamples, renderer::kGiMaxTargetSamples);
    range("maxBounces", settings.maxBounces, renderer::kGiMaxBounces);
}

RenderingSettings readRenderingSettings(const Json& value) {
    RenderingSettings settings;
    if (!value.contains("rendering")) return settings;
    const auto& rendering = value.at("rendering");
    if (!rendering.is_object()) throw std::runtime_error("rendering must be an object.");
    const auto read = [&rendering](const char* key, bool& result) {
        if (!rendering.contains(key)) return;
        if (!rendering.at(key).is_boolean())
            throw std::runtime_error(std::string("rendering.") + key + " must be a boolean.");
        result = rendering.at(key).get<bool>();
    };
    read("hardwareRayTracing", settings.hardwareRayTracing);
    read("hardwareRayTracingReflections", settings.hardwareRayTracingReflections);
    read("hardwareRayTracingShadows", settings.hardwareRayTracingShadows);
    if (rendering.contains("globalIllumination")) {
        const auto& gi = rendering.at("globalIllumination");
        if (!gi.is_object()) throw std::runtime_error("rendering.globalIllumination must be an object.");
        if (gi.contains("method")) {
            if (!gi.at("method").is_string())
                throw std::runtime_error("rendering.globalIllumination.method must be disabled or pathTracing.");
            const auto& method = gi.at("method").get_ref<const std::string&>();
            if (method == "disabled") settings.globalIllumination.method = renderer::GlobalIlluminationMethod::Disabled;
            else if (method == "pathTracing") settings.globalIllumination.method = renderer::GlobalIlluminationMethod::PathTracing;
            else throw std::runtime_error("rendering.globalIllumination.method must be disabled or pathTracing.");
        }
        if (gi.contains("mode")) {
            if (!gi.at("mode").is_string())
                throw std::runtime_error("rendering.globalIllumination.mode must be progressive or realtime.");
            const auto& mode = gi.at("mode").get_ref<const std::string&>();
            if (mode == "progressive") settings.globalIllumination.mode = renderer::GlobalIlluminationMode::Progressive;
            else if (mode == "realtime") settings.globalIllumination.mode = renderer::GlobalIlluminationMode::Realtime;
            else throw std::runtime_error("rendering.globalIllumination.mode must be progressive or realtime.");
        }
        const auto readContribution = [&gi](const char* key, bool& result) {
            if (!gi.contains(key)) return;
            if (!gi.at(key).is_boolean())
                throw std::runtime_error(std::string("rendering.globalIllumination.") + key + " must be a boolean.");
            result = gi.at(key).get<bool>();
        };
        readContribution("indirectDiffuse", settings.globalIllumination.indirectDiffuse);
        readContribution("indirectSpecular", settings.globalIllumination.indirectSpecular);
        const auto readCount = [&gi](const char* key, std::uint32_t& result, std::uint32_t maximum) {
            if (!gi.contains(key)) return;
            const auto& count = gi.at(key);
            const auto reject = [key, maximum] {
                throw std::runtime_error(std::string("rendering.globalIllumination.") + key +
                    " must be an integer from 1 to " + std::to_string(maximum) + ".");
            };
            if (!count.is_number_integer()) reject();
            std::uint64_t integer = 0;
            if (count.is_number_unsigned()) integer = count.get<std::uint64_t>();
            else {
                const auto signedInteger = count.get<std::int64_t>();
                if (signedInteger < 1) reject();
                integer = static_cast<std::uint64_t>(signedInteger);
            }
            if (integer < 1 || integer > maximum) reject();
            result = static_cast<std::uint32_t>(integer);
        };
        readCount("samplesPerFrame", settings.globalIllumination.samplesPerFrame, renderer::kGiMaxSamplesPerFrame);
        readCount("targetSamples", settings.globalIllumination.targetSamples, renderer::kGiMaxTargetSamples);
        readCount("maxBounces", settings.globalIllumination.maxBounces, renderer::kGiMaxBounces);
    }
    return settings;
}

std::string pathUtf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
fs::path pathFromUtf8(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

bool sameComponent(const fs::path& left, const fs::path& right) {
#ifdef _WIN32
    auto a = left.native();
    auto b = right.native();
    std::transform(a.begin(), a.end(), a.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    std::transform(b.begin(), b.end(), b.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return a == b;
#else
    return left == right;
#endif
}
bool isWithin(const fs::path& folder, const fs::path& path) {
    auto folderIt = folder.begin();
    auto pathIt = path.begin();
    for (; folderIt != folder.end(); ++folderIt, ++pathIt)
        if (pathIt == path.end() || !sameComponent(*folderIt, *pathIt)) return false;
    return true;
}
fs::path projectPath(const Json& value, const char* key, const fs::path& projectRoot) {
    const auto text = value.at(key).get<std::string>();
    const fs::path relative = pathFromUtf8(text);
    if (text.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
        throw std::runtime_error(std::string(key) + " must be a nonempty project-relative path.");
    const auto resolved = fs::weakly_canonical(projectRoot / relative);
    if (!isWithin(projectRoot, resolved))
        throw std::runtime_error(std::string(key) + " must remain inside the project directory (including resolved links).");
    return resolved;
}
bool isManifest(const fs::path& path) {
    auto extension = pathUtf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".raproject";
}

bool resolveManifest(const fs::path& folderOrManifest, fs::path& manifest, std::string& error) {
    if (folderOrManifest.empty()) { error = "Project path is empty."; return false; }
    fs::path candidate = fs::absolute(folderOrManifest);
    if (fs::is_directory(candidate)) {
        std::vector<fs::path> manifests;
        for (const auto& entry : fs::directory_iterator(candidate))
            if (entry.is_regular_file() && isManifest(entry.path())) manifests.push_back(entry.path());
        if (manifests.size() != 1) {
            error = "Project directory must contain exactly one .raproject manifest: " + pathUtf8(candidate);
            return false;
        }
        candidate = manifests.front();
    }
    if (!isManifest(candidate) || !fs::is_regular_file(candidate)) {
        error = "Project manifest does not exist or is not a .raproject file: " + pathUtf8(candidate);
        return false;
    }
    std::error_code ec;
    manifest = fs::canonical(candidate, ec);
    if (ec) { error = "Project manifest cannot be resolved: " + pathUtf8(candidate); return false; }
    return true;
}

std::string readEngineBindingField(const Json& value, const char* key, const char* nestedKey) {
    if (value.contains(key)) {
        if (!value.at(key).is_string()) throw std::runtime_error(std::string(key) + " must be a string.");
        return value.at(key).get<std::string>();
    }
    if (value.contains("engineBinding")) {
        const auto& binding = value.at("engineBinding");
        if (!binding.is_object() || !binding.contains(nestedKey))
            throw std::runtime_error("engineBinding must contain " + std::string(nestedKey) + ".");
        if (!binding.at(nestedKey).is_string())
            throw std::runtime_error(std::string("engineBinding.") + nestedKey + " must be a string.");
        return binding.at(nestedKey).get<std::string>();
    }
    return {};
}

fs::path engineManifestPath(const fs::path& root) {
    return root / fs::path(kEngineManifestFileName);
}

std::string requiredString(const Json& value, const char* key, const char* description) {
    if (!value.contains(key) || !value.at(key).is_string())
        throw std::runtime_error(std::string(description) + " must be a string.");
    const auto text = value.at(key).get<std::string>();
    if (text.empty()) throw std::runtime_error(std::string(description) + " cannot be empty.");
    return text;
}

std::string stableGuid(const std::string& seed) {
    // A stable identifier keeps generated solutions quiet in source control.
    // FNV-1a is sufficient here: this is an IDE container identifier, not a
    // security or content hash.
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto byte : seed) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ull;
    }
    std::uint64_t second = hash ^ 0x9e3779b97f4a7c15ull;
    std::ostringstream stream;
    stream << '{' << std::uppercase << std::hex << std::setfill('0')
           << std::setw(8) << static_cast<std::uint32_t>(hash >> 32)
           << std::setw(4) << static_cast<std::uint16_t>(hash >> 16)
           << std::setw(4) << static_cast<std::uint16_t>(hash)
           << std::setw(4) << static_cast<std::uint16_t>(second >> 48)
           << std::setw(12) << (second & 0x0000ffffffffffffull) << '}';
    return stream.str();
}

std::string solutionGuid(const ProjectPaths& project) {
    return stableGuid(pathUtf8(project.projectRoot) + "\n" + project.name);
}

std::string solutionPath(const fs::path& path) {
    auto text = pathUtf8(path);
    std::replace(text.begin(), text.end(), '/', '\\');
    return text;
}

std::string readProjectGuid(const fs::path& path) {
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("Cannot read engine project: " + pathUtf8(path));
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    constexpr std::string_view open = "<ProjectGuid>";
    const auto start = text.find(open);
    if (start == std::string::npos)
        throw std::runtime_error("Engine project has no ProjectGuid: " + pathUtf8(path));
    const auto begin = start + open.size();
    const auto end = text.find("</ProjectGuid>", begin);
    if (end == std::string::npos)
        throw std::runtime_error("Engine project has a malformed ProjectGuid: " + pathUtf8(path));
    return text.substr(begin, end - begin);
}

std::string joinSolutionPath(const std::string& base, const char* name) {
    if (base.empty() || base == ".") return name;
    return base + "\\" + name;
}

EngineCompatibilityReport compatibilityFor(const ProjectPaths& project, const EngineIdentity& candidate) {
    EngineCompatibilityReport report;
    report.canClose = true;
    if (!project.hasEngineBinding()) {
        report.state = EngineCompatibility::LegacyProject;
        report.message = "Project has no engine binding and can be upgraded to this engine.";
        report.canForceUpdate = candidate.manifestPresent;
        report.canSwitchEngine = candidate.manifestPresent;
        return report;
    }
    if (project.engineVersion.empty() || project.engineHash.empty()) {
        report.state = EngineCompatibility::InvalidBinding;
        report.message = "Project engine binding must include both engineVersion and engineHash.";
        return report;
    }
    if (!candidate.manifestPresent) {
        report.state = EngineCompatibility::EngineManifestMissing;
        report.message = "Selected engine has no Engine.raeengine identity manifest.";
        return report;
    }
    if (candidate.version != project.engineVersion) {
        report.state = EngineCompatibility::VersionMismatch;
        report.message = "Project requires engine version " + project.engineVersion + ", but this engine is " + candidate.version + ".";
        report.canForceUpdate = true;
        report.canSwitchEngine = true;
        return report;
    }
    if (candidate.hash != project.engineHash) {
        report.state = EngineCompatibility::HashMismatch;
        report.message = "Project requires engine hash " + project.engineHash + ", but this engine is " + candidate.hash + ".";
        report.canForceUpdate = true;
        report.canSwitchEngine = true;
        return report;
    }
    report.state = EngineCompatibility::Compatible;
    report.message = "Project and engine identities match.";
    return report;
}
} // namespace

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // std::getenv is intentional here; values are only read, never written.
#endif
std::filesystem::path defaultEngineRegistryPath() {
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
        return fs::path(pathFromUtf8(local)) / "RAEngine" / "engines.json";
    if (const char* profile = std::getenv("USERPROFILE"); profile && *profile)
        return fs::path(pathFromUtf8(profile)) / ".raengine" / "engines.json";
#else
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && *config)
        return fs::path(config) / "raengine" / "engines.json";
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".config" / "raengine" / "engines.json";
#endif
    return fs::path("engines.json");
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

bool loadEngineRegistry(const std::filesystem::path& path, EngineRegistry& result, std::string& error) {
    try {
        EngineRegistry staged;
        if (!fs::exists(path)) { result = std::move(staged); error.clear(); return true; }
        std::ifstream stream(path);
        if (!stream) throw std::runtime_error("Cannot read engine registry: " + pathUtf8(path));
        const auto value = Json::parse(stream);
        if (!value.contains("format") || value.at("format") != "RAEngine.EngineRegistry" ||
            (value.contains("version") && value.at("version") != 1))
            throw std::runtime_error("Unsupported engine registry format.");
        if (!value.contains("engines") || !value.at("engines").is_array())
            throw std::runtime_error("Engine registry must contain an engines array.");
        for (const auto& item : value.at("engines")) {
            if (!item.is_object() || !item.contains("association") || !item.contains("root"))
                throw std::runtime_error("Engine registry entries need association and root.");
            EngineRegistryEntry entry;
            entry.association = item.at("association").get<std::string>();
            entry.root = fs::absolute(fs::path(pathFromUtf8(item.at("root").get<std::string>())));
            if (entry.association.empty() || entry.root.empty())
                throw std::runtime_error("Engine registry entry has an empty association or root.");
            staged.engines.push_back(std::move(entry));
        }
        result = std::move(staged);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool saveEngineRegistry(const std::filesystem::path& path, const EngineRegistry& registry, std::string& error) {
    try {
        if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
        Json value = {{"format", "RAEngine.EngineRegistry"}, {"version", 1}, {"engines", Json::array()}};
        for (const auto& entry : registry.engines)
            value["engines"].push_back({{"association", entry.association}, {"root", pathUtf8(entry.root)}});
        const auto temporary = path.string() + ".tmp";
        {
            std::ofstream stream(temporary, std::ios::trunc);
            if (!stream) throw std::runtime_error("Cannot write engine registry: " + pathUtf8(path));
            stream << value.dump(2) << '\n';
        }
        std::error_code ec;
        fs::rename(temporary, path, ec);
        if (ec) {
            fs::remove(path, ec);
            ec.clear();
            fs::rename(temporary, path, ec);
        }
        if (ec) throw std::runtime_error("Cannot replace engine registry: " + ec.message());
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

void registerEngineInRegistry(EngineRegistry& registry, std::string_view association, std::filesystem::path root) {
    if (association.empty() || root.empty()) return;
    root = fs::absolute(root).lexically_normal();
    registry.engines.erase(std::remove_if(registry.engines.begin(), registry.engines.end(),
        [&](const EngineRegistryEntry& entry) { return entry.association == association || entry.root == root; }),
        registry.engines.end());
    registry.engines.push_back(EngineRegistryEntry{std::string(association), std::move(root)});
}

bool findEngineByAssociation(const EngineRegistry& registry, std::string_view association, EngineRegistryEntry& result) {
    for (const auto& entry : registry.engines)
        if (entry.association == association) { result = entry; return true; }
    return false;
}

bool loadProjectManifestInfo(const std::filesystem::path& folderOrManifest, ProjectManifestInfo& result, std::string& error) {
    try {
        fs::path manifest;
        if (!resolveManifest(folderOrManifest, manifest, error)) return false;
        std::ifstream stream(manifest);
        if (!stream) throw std::runtime_error("Cannot read project manifest: " + pathUtf8(manifest));
        const auto value = Json::parse(stream);
        if (value.at("format") != "RAEngine.Project" ||
            (value.at("version") != 1 && value.at("version") != 2 && value.at("version") != 3))
            throw std::runtime_error("Unsupported project format or version.");
        ProjectManifestInfo staged;
        staged.manifest = manifest;
        staged.projectRoot = manifest.parent_path();
        staged.name = value.at("name").get<std::string>();
        if (staged.name.empty()) throw std::runtime_error("Project name cannot be empty.");
        staged.engineVersion = readEngineBindingField(value, "engineVersion", "version");
        staged.engineHash = readEngineBindingField(value, "engineHash", "hash");
        if (staged.engineVersion.empty() != staged.engineHash.empty())
            throw std::runtime_error("Project engine binding must include both engineVersion and engineHash.");
        if (value.contains("engineAssociation")) {
            if (!value.at("engineAssociation").is_string())
                throw std::runtime_error("engineAssociation must be a string.");
            staged.engineAssociation = value.at("engineAssociation").get<std::string>();
        }
        result = std::move(staged);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool loadEngineIdentity(const std::filesystem::path& engineRoot, EngineIdentity& result, std::string& error) {
    try {
        if (engineRoot.empty()) throw std::runtime_error("Engine root is empty.");
        const auto root = fs::canonical(engineRoot);
        if (!fs::is_directory(root)) throw std::runtime_error("Engine root is not a directory.");
        const auto manifest = engineManifestPath(root);
        if (!fs::is_regular_file(manifest))
            throw std::runtime_error("Engine identity manifest does not exist: " + pathUtf8(manifest));
        std::ifstream stream(manifest);
        if (!stream) throw std::runtime_error("Cannot read engine identity manifest: " + pathUtf8(manifest));
        const auto value = Json::parse(stream);
        if (!value.contains("format") || value.at("format") != "RAEngine.Engine" ||
            !value.contains("version") || value.at("version") != 1)
            throw std::runtime_error("Unsupported engine identity format or version.");
        EngineIdentity staged;
        staged.root = root;
        staged.manifest = fs::canonical(manifest);
        staged.name = value.contains("name") ? value.at("name").get<std::string>() : "RestArtEngine";
        staged.version = requiredString(value, "engineVersion", "engineVersion");
        staged.hash = requiredString(value, "engineHash", "engineHash");
        staged.manifestPresent = true;
        result = std::move(staged);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

EngineCompatibilityReport checkEngineCompatibility(const ProjectPaths& project) {
    return compatibilityFor(project, project.engine);
}

EngineCompatibilityReport checkEngineCompatibility(const ProjectPaths& project, const EngineIdentity& candidate) {
    return compatibilityFor(project, candidate);
}

bool writeEngineManifest(const std::filesystem::path& engineRoot, std::string_view name,
                         std::string_view version, std::string_view hash, std::string& error) {
    try {
        const auto root = fs::canonical(engineRoot);
        if (!fs::is_directory(root)) throw std::runtime_error("Engine root is not a directory.");
        if (version.empty() || hash.empty()) throw std::runtime_error("Engine version and hash cannot be empty.");
        Json value = {{"format", "RAEngine.Engine"}, {"version", 1}, {"name", std::string(name)},
                      {"engineVersion", std::string(version)}, {"engineHash", std::string(hash)}};
        const auto manifest = engineManifestPath(root);
        const auto temporary = manifest.string() + ".tmp";
        {
            std::ofstream stream(temporary, std::ios::trunc);
            if (!stream) throw std::runtime_error("Cannot write engine identity manifest: " + pathUtf8(manifest));
            stream << value.dump(2) << '\n';
        }
        std::error_code ec;
        fs::rename(temporary, manifest, ec);
        if (ec) {
            fs::remove(manifest, ec);
            ec.clear();
            fs::rename(temporary, manifest, ec);
        }
        if (ec) throw std::runtime_error("Cannot replace engine identity manifest: " + ec.message());
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool updateProjectEngineBinding(const std::filesystem::path& folderOrManifest,
                                const EngineIdentity& engine, std::string& error) {
    try {
        if (!engine.manifestPresent || engine.root.empty() || engine.version.empty() || engine.hash.empty())
            throw std::runtime_error("Cannot bind a project to an engine without a complete identity.");
        fs::path manifest = fs::absolute(folderOrManifest);
        if (fs::is_directory(manifest)) {
            std::vector<fs::path> candidates;
            for (const auto& entry : fs::directory_iterator(manifest))
                if (entry.is_regular_file() && isManifest(entry.path())) candidates.push_back(entry.path());
            if (candidates.size() != 1)
                throw std::runtime_error("Project directory must contain exactly one .raproject manifest: " + pathUtf8(manifest));
            manifest = candidates.front();
        }
        if (!isManifest(manifest) || !fs::is_regular_file(manifest))
            throw std::runtime_error("Project manifest does not exist or is not a .raproject file: " + pathUtf8(manifest));
        manifest = fs::canonical(manifest);
        std::ifstream input(manifest);
        if (!input) throw std::runtime_error("Cannot read project manifest: " + pathUtf8(manifest));
        auto value = Json::parse(input);
        input.close();
        if (value.at("format") != "RAEngine.Project") throw std::runtime_error("Unsupported project format.");
        value["version"] = 3;
        // Keep the binding flat and human-readable.  The loader also accepts
        // the nested form so external project managers can migrate their own files.
        value["engineVersion"] = engine.version;
        value["engineHash"] = engine.hash;
        // The named association defaults to the engine version; the Project Manager
        // resolves it to an install path while engineRoot stays the cache.
        value["engineAssociation"] = engine.version;
        // engineRoot is resolved relative to the project manifest's folder.
        // `relative(path, base)` returns the path to the engine from the
        // project directory. Keep the engine as the first argument so nested
        // projects resolve to links such as ../../Engine.
        // A relative path cannot express another volume, so in that case keep
        // only the association: the loader resolves it through the registry
        // instead of persisting a machine-specific absolute path.
        const auto relative = fs::relative(engine.root, manifest.parent_path());
        if (relative.empty())
            value.erase("engineRoot");
        else
            value["engineRoot"] = pathUtf8(relative);
        const auto temporary = manifest.string() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output) throw std::runtime_error("Cannot write project manifest: " + pathUtf8(manifest));
            output << value.dump(2) << '\n';
        }
        std::error_code ec;
        fs::rename(temporary, manifest, ec);
        if (ec) {
            fs::remove(manifest, ec);
            ec.clear();
            fs::rename(temporary, manifest, ec);
        }
        if (ec) throw std::runtime_error("Cannot replace project manifest: " + ec.message());
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool recoverProject(const std::filesystem::path& folderOrManifest,
                    const std::filesystem::path& preferredEngineRoot,
                    ProjectPaths& result, std::string& error) {
    try {
        ProjectManifestInfo info;
        std::string infoError;
        if (!loadProjectManifestInfo(folderOrManifest, info, infoError))
            throw std::runtime_error(infoError);
        if (info.engineAssociation.empty() && info.engineVersion.empty())
            throw std::runtime_error("Project has no engine binding to recover.");
        std::vector<EngineIdentity> candidates;
        const auto addCandidate = [&candidates](const std::filesystem::path& root) {
            if (root.empty()) return;
            const auto absolute = fs::weakly_canonical(root);
            for (const auto& candidate : candidates)
                if (candidate.root == absolute) return;
            EngineIdentity identity;
            std::string identityError;
            if (loadEngineIdentity(absolute, identity, identityError)) candidates.push_back(std::move(identity));
        };
        // The running engine is the best candidate: it needs no registry entry
        // and it is the copy that will actually render the project.
        addCandidate(preferredEngineRoot);
        if (!info.engineAssociation.empty()) {
            EngineRegistry registry;
            std::string registryError;
            if (loadEngineRegistry(defaultEngineRegistryPath(), registry, registryError)) {
                EngineRegistryEntry entry;
                if (findEngineByAssociation(registry, info.engineAssociation, entry)) addCandidate(entry.root);
            }
        }
        // Only rebind to an engine that matches the recorded binding. Switching
        // to a different version stays an explicit project-manager action.
        const auto matches = [&info](const EngineIdentity& identity) {
            if (identity.version.empty()) return false;
            if (!info.engineAssociation.empty() && identity.version == info.engineAssociation) return true;
            return !info.engineVersion.empty() && identity.version == info.engineVersion;
        };
        const auto match = std::find_if(candidates.begin(), candidates.end(), matches);
        if (match == candidates.end()) {
            const auto requested = info.engineAssociation.empty() ? info.engineVersion : info.engineAssociation;
            throw std::runtime_error("No available engine matches '" + requested +
                "'. Open the project in the project manager to switch engine versions.");
        }
        std::string bindingError;
        if (!updateProjectEngineBinding(info.manifest, *match, bindingError)) throw std::runtime_error(bindingError);
        std::string loadError;
        if (!loadProject(info.manifest, result, loadError)) throw std::runtime_error(loadError);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool writeProjectBuildFiles(const ProjectPaths& project, ProjectBuildFiles& result,
                            std::string& error) {
    try {
        if (project.projectRoot.empty() || !fs::is_directory(project.projectRoot))
            throw std::runtime_error("Project root is not a directory.");
        if (project.manifest.empty() || !fs::is_regular_file(project.manifest))
            throw std::runtime_error("Project manifest does not exist.");
        if (project.name.empty()) throw std::runtime_error("Project name cannot be empty.");
        for (const auto character : project.name)
            if (character == '/' || character == '\\' || character == ':' || character == '"' ||
                static_cast<unsigned char>(character) < 0x20)
                throw std::runtime_error("Project name contains an invalid path character.");
        if (project.engineRoot.empty() || project.engineVersion.empty() || project.engineHash.empty())
            throw std::runtime_error("Project must have a complete engine binding before generating build files.");

        const auto solution = project.projectRoot / (project.name + ".sln");
        const auto buildManifest = project.projectRoot / "RestArtEngineProject.json";
        const auto projectItemGuid = solutionGuid(project);
        const auto manifestName = project.manifest.filename();
        const auto engineRelative = fs::relative(project.engineRoot, project.projectRoot);

        // When the linked engine ships its MSBuild project, reference it so
        // opening this solution also opens and builds the engine source.  The
        // manifest items stay in a solution-folder for quick access.
        const auto engineVcxproj = project.engineRoot / "RestArtEngine.vcxproj";
        const bool engineSources = fs::is_regular_file(engineVcxproj);
        std::string engineGuid;
        if (engineSources) engineGuid = readProjectGuid(engineVcxproj);

        std::ostringstream sln;
        sln << "Microsoft Visual Studio Solution File, Format Version 12.00\n"
            << "# Visual Studio Version 17\n"
            << "VisualStudioVersion = 17.0.31903.59\n"
            << "MinimumVisualStudioVersion = 10.0.40219.1\n";
        if (engineSources) {
            const auto engineProjectPath = joinSolutionPath(solutionPath(engineRelative), "RestArtEngine.vcxproj");
            sln << "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"RestArtEngine\", \""
                << engineProjectPath << "\", \"" << engineGuid << "\"\n"
                << "EndProject\n";
        }
        sln << "Project(\"{2150E333-8FDC-42A3-9474-1A3956D46DE8}\") = \""
            << project.name << "\", \".\", \"" << projectItemGuid << "\"\n"
            << "\tProjectSection(SolutionItems) = preProject\n"
            << "\t\t" << solutionPath(manifestName) << " = " << solutionPath(manifestName) << "\n"
            << "\t\tRestArtEngineProject.json = RestArtEngineProject.json\n"
            << "\tEndProjectSection\n"
            << "EndProject\n"
            << "Global\n"
            << "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n"
            << "\t\tDebug|x64 = Debug|x64\n"
            << "\t\tRelease|x64 = Release|x64\n"
            << "\tEndGlobalSection\n"
            << "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\n";
        if (engineSources) {
            sln << "\t\t" << engineGuid << ".Debug|x64.ActiveCfg = Debug|x64\n"
                << "\t\t" << engineGuid << ".Debug|x64.Build.0 = Debug|x64\n"
                << "\t\t" << engineGuid << ".Release|x64.ActiveCfg = Release|x64\n"
                << "\t\t" << engineGuid << ".Release|x64.Build.0 = Release|x64\n";
        }
        sln << "\tEndGlobalSection\n"
            << "\tGlobalSection(SolutionProperties) = preSolution\n"
            << "\t\tHideSolutionNode = FALSE\n"
            << "\tEndGlobalSection\n"
            << "\tGlobalSection(ExtensibilityGlobals) = postSolution\n"
            << "\t\tSolutionGuid = " << stableGuid(pathUtf8(project.projectRoot) + "\n" + project.name + "\nsolution") << "\n"
            << "\tEndGlobalSection\n"
            << "EndGlobal\n";

        Json build = {
            {"format", "RAEngine.ProjectBuild"},
            {"version", 1},
            {"projectManifest", pathUtf8(manifestName)},
            {"solution", pathUtf8(solution.filename())},
            {"engineRoot", pathUtf8(engineRelative)},
            {"engineAssociation", project.engineAssociation},
            {"engineVersion", project.engineVersion},
            {"engineHash", project.engineHash},
            {"engineSources", engineSources},
            {"build", {
                {"description", "Configure and build the RestArtEngine solution from the linked engine root."},
                {"engineSolution", "RestArtEngine.sln"},
                {"engineExecutable", "RestArtEngine.exe"},
                {"cmakeSource", pathUtf8(engineRelative)}
            }}
        };

        const auto writeText = [](const fs::path& path, const std::string& text) {
            const auto temporary = path.string() + ".tmp";
            {
                std::ofstream stream(temporary, std::ios::trunc);
                if (!stream) throw std::runtime_error("Cannot write project build file: " + pathUtf8(path));
                stream << text;
            }
            std::error_code ec;
            fs::rename(temporary, path, ec);
            if (ec) {
                fs::remove(path, ec);
                ec.clear();
                fs::rename(temporary, path, ec);
            }
            if (ec) throw std::runtime_error("Cannot replace project build file: " + ec.message());
        };
        writeText(solution, sln.str());
        writeText(buildManifest, build.dump(2) + "\n");
        result.solution = solution;
        result.buildManifest = buildManifest;
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool loadProject(const std::filesystem::path& folderOrManifest, ProjectPaths& result, std::string& error) {
    try {
        if (folderOrManifest.empty()) throw std::runtime_error("Project path is empty.");
        fs::path manifest = fs::absolute(folderOrManifest);
        if (fs::is_directory(manifest)) {
            std::vector<fs::path> candidates;
            for (const auto& entry : fs::directory_iterator(manifest))
                if (entry.is_regular_file() && isManifest(entry.path())) candidates.push_back(entry.path());
            if (candidates.size() != 1)
                throw std::runtime_error("Project directory must contain exactly one .raproject manifest: " + pathUtf8(manifest));
            manifest = candidates.front();
        }
        if (!isManifest(manifest) || !fs::is_regular_file(manifest))
            throw std::runtime_error("Project manifest does not exist or is not a .raproject file: " + pathUtf8(manifest));
        manifest = fs::canonical(manifest);
        std::ifstream stream(manifest);
        if (!stream) throw std::runtime_error("Cannot read project manifest: " + pathUtf8(manifest));
        const auto value = Json::parse(stream);
        if (value.at("format") != "RAEngine.Project" ||
            (value.at("version") != 1 && value.at("version") != 2 && value.at("version") != 3))
            throw std::runtime_error("Unsupported project format or version.");
        ProjectPaths staged;
        staged.manifest = manifest;
        staged.rendering = readRenderingSettings(value);
        staged.name = value.at("name").get<std::string>();
        if (staged.name.empty()) throw std::runtime_error("Project name cannot be empty.");
        staged.projectRoot = manifest.parent_path();
        const auto readBinding = [&value](const char* key, const char* nestedKey) -> std::string {
            if (value.contains(key)) {
                if (!value.at(key).is_string()) throw std::runtime_error(std::string(key) + " must be a string.");
                return value.at(key).get<std::string>();
            }
            if (value.contains("engineBinding")) {
                const auto& binding = value.at("engineBinding");
                if (!binding.is_object() || !binding.contains(nestedKey))
                    throw std::runtime_error("engineBinding must contain " + std::string(nestedKey) + ".");
                if (!binding.at(nestedKey).is_string())
                    throw std::runtime_error(std::string("engineBinding.") + nestedKey + " must be a string.");
                return binding.at(nestedKey).get<std::string>();
            }
            return {};
        };
        staged.engineVersion = readBinding("engineVersion", "version");
        staged.engineHash = readBinding("engineHash", "hash");
        if (staged.engineVersion.empty() != staged.engineHash.empty())
            throw std::runtime_error("Project engine binding must include both engineVersion and engineHash.");
        staged.engineAssociation = readBinding("engineAssociation", "association");
        // engineRoot is a cache for direct --project opens. A binding created
        // from another volume cannot be expressed relatively, so such a
        // manifest keeps only engineAssociation and the registry resolves it.
        if (value.contains("engineRoot")) {
            if (!value.at("engineRoot").is_string())
                throw std::runtime_error("engineRoot must be a string.");
            const auto engine = value.at("engineRoot").get<std::string>();
            if (!engine.empty())
                staged.engineRoot = fs::weakly_canonical(staged.projectRoot / pathFromUtf8(engine));
        }
        if (staged.engineRoot.empty()) {
            if (staged.engineAssociation.empty())
                throw std::runtime_error("Project engineRoot is missing and there is no engineAssociation to resolve it.");
            EngineRegistry registry;
            std::string registryError;
            EngineRegistryEntry entry;
            if (!loadEngineRegistry(defaultEngineRegistryPath(), registry, registryError))
                throw std::runtime_error(registryError);
            if (!findEngineByAssociation(registry, staged.engineAssociation, entry))
                throw std::runtime_error("Project engineRoot is missing and no installed engine matches association '" +
                    staged.engineAssociation + "'.");
            staged.engineRoot = fs::weakly_canonical(entry.root);
        }
        if (!fs::is_regular_file(staged.engineRoot / "Shaders" / "Private" / "Scene.hlsl"))
            throw std::runtime_error("engineRoot must reference an engine containing Shaders/Private/Scene.hlsl.");
        const auto identityManifest = engineManifestPath(staged.engineRoot);
        if (fs::is_regular_file(identityManifest)) {
            std::string identityError;
            if (!loadEngineIdentity(staged.engineRoot, staged.engine, identityError))
                throw std::runtime_error(identityError);
        } else {
            staged.engine.root = staged.engineRoot;
            staged.engine.manifest = identityManifest;
            staged.engine.manifestPresent = false;
        }
        staged.contentRoot = projectPath(value, "contentRoot", staged.projectRoot);
        staged.shaderRoot = projectPath(value, "shaderRoot", staged.projectRoot);
        staged.defaultScene = projectPath(value, "defaultScene", staged.projectRoot);
        if (!fs::is_directory(staged.contentRoot)) throw std::runtime_error("Project contentRoot directory does not exist.");
        if (!fs::is_directory(staged.shaderRoot)) throw std::runtime_error("Project shaderRoot directory does not exist.");
        if (!fs::is_regular_file(staged.defaultScene)) throw std::runtime_error("Project defaultScene file does not exist.");
        result = std::move(staged);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool saveProjectRenderingSettings(const std::filesystem::path& folderOrManifest,
                                  const RenderingSettings& settings, std::string& error) {
    fs::path temporary;
    bool temporaryOwned = false;
    try {
        validateGlobalIllumination(settings.globalIllumination);
        fs::path manifest;
        if (!resolveManifest(folderOrManifest, manifest, error)) return false;
        std::ifstream input(manifest, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read project manifest: " + pathUtf8(manifest));
        auto value = Json::parse(input);
        input.close();
        if (value.at("format") != "RAEngine.Project" ||
            (value.at("version") != 1 && value.at("version") != 2 && value.at("version") != 3))
            throw std::runtime_error("Unsupported project format or version.");
        // Validate the existing object before editing it; a malformed document
        // must not be silently converted into a different project schema.
        (void)readRenderingSettings(value);
        auto& rendering = value["rendering"];
        if (rendering.is_null()) rendering = Json::object();
        rendering["hardwareRayTracing"] = settings.hardwareRayTracing;
        rendering["hardwareRayTracingReflections"] = settings.hardwareRayTracingReflections;
        rendering["hardwareRayTracingShadows"] = settings.hardwareRayTracingShadows;
        auto& gi = rendering["globalIllumination"];
        if (gi.is_null()) gi = Json::object();
        gi["method"] = settings.globalIllumination.method == renderer::GlobalIlluminationMethod::PathTracing
            ? "pathTracing" : "disabled";
        gi["mode"] = settings.globalIllumination.mode == renderer::GlobalIlluminationMode::Realtime
            ? "realtime" : "progressive";
        gi["indirectDiffuse"] = settings.globalIllumination.indirectDiffuse;
        gi["indirectSpecular"] = settings.globalIllumination.indirectSpecular;
        gi["samplesPerFrame"] = settings.globalIllumination.samplesPerFrame;
        gi["targetSamples"] = settings.globalIllumination.targetSamples;
        gi["maxBounces"] = settings.globalIllumination.maxBounces;
        const auto text = value.dump(2) + '\n';
        static std::atomic<std::uint64_t> sequence{0};
        temporary = manifest;
        temporary += ".tmp-settings-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
#ifdef _WIN32
        const auto file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot create temporary project settings file (Win32 " +
                std::to_string(GetLastError()) + ").");
        temporaryOwned = true;
        bool written = true;
        std::size_t offset = 0;
        while (offset < text.size()) {
            const auto amount = static_cast<DWORD>(std::min<std::size_t>(text.size() - offset, 1u << 20));
            DWORD count = 0;
            if (!WriteFile(file, text.data() + offset, amount, &count, nullptr) || count != amount) {
                written = false;
                break;
            }
            offset += count;
        }
        if (written) written = FlushFileBuffers(file) != FALSE;
        const auto writeError = written ? ERROR_SUCCESS : GetLastError();
        const bool closed = CloseHandle(file) != FALSE;
        if (!written || !closed)
            throw std::runtime_error("Cannot write project settings (Win32 " +
                std::to_string(written ? GetLastError() : writeError) + ").");
        if (!MoveFileExW(temporary.c_str(), manifest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace project settings (Win32 " +
                std::to_string(GetLastError()) + "). Original project retained.");
#else
        const auto file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (file < 0) throw std::runtime_error("Cannot create temporary project settings file.");
        temporaryOwned = true;
        bool written = true;
        std::size_t offset = 0;
        while (offset < text.size()) {
            const auto count = ::write(file, text.data() + offset, text.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { written = false; break; }
            offset += static_cast<std::size_t>(count);
        }
        if (written) written = ::fsync(file) == 0;
        const bool closed = ::close(file) == 0;
        if (!written || !closed) throw std::runtime_error("Cannot write project settings.");
        fs::rename(temporary, manifest);
#endif
        temporary.clear();
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        if (temporaryOwned && !temporary.empty()) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
        }
        error = exception.what();
        return false;
    }
}
} // namespace rae::project
