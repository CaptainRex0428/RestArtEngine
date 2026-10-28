#pragma once

#include <RestArtEngine/Renderer/GlobalIllumination.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rae::project {
// Every engine installation has one small identity manifest at its root.  The
// manifest is deliberately independent from the build system so projects can
// bind to a released or privately built engine copy.
inline constexpr std::string_view kEngineManifestFileName = "Engine.raeengine";

struct EngineIdentity {
    std::filesystem::path root;
    std::filesystem::path manifest;
    std::string name;
    std::string version;
    std::string hash;
    bool manifestPresent = false;
};

enum class EngineCompatibility {
    Compatible,
    LegacyProject,          // A v1 project without an engine binding.
    EngineManifestMissing,
    VersionMismatch,
    HashMismatch,
    InvalidBinding,
};

struct EngineCompatibilityReport {
    EngineCompatibility state = EngineCompatibility::InvalidBinding;
    std::string message;
    bool canForceUpdate = false;
    bool canSwitchEngine = false;
    bool canClose = true;

    [[nodiscard]] bool compatible() const noexcept {
        return state == EngineCompatibility::Compatible || state == EngineCompatibility::LegacyProject;
    }
};

// Requested project features, independent of the device that opens the project.
// Capability detection must not rewrite these values when a feature is unavailable.
struct RenderingSettings {
    bool hardwareRayTracing = false;
    bool hardwareRayTracingReflections = true;
    bool hardwareRayTracingShadows = true;
    renderer::GlobalIlluminationSettings globalIllumination;
    bool operator==(const RenderingSettings&) const = default;
};

struct ProjectPaths {
    std::filesystem::path projectRoot;
    std::filesystem::path engineRoot;
    std::filesystem::path contentRoot;
    std::filesystem::path shaderRoot;
    std::filesystem::path defaultScene;
    std::filesystem::path manifest;
    std::string name;
    RenderingSettings rendering;
    // The identity discovered at engineRoot.  Legacy engines may not have a
    // manifest, in which case manifestPresent is false and the project still
    // loads for backwards compatibility.
    EngineIdentity engine;
    // Identity requested by the project manifest.  Empty values mean this is
    // a legacy project and it can be upgraded by the project manager.
    std::string engineVersion;
    std::string engineHash;
    // Named engine association (UE-style). The Project Manager resolves it through
    // the engine registry; engineRoot remains a concrete fallback/cache so a
    // project can still be opened directly with --project.
    std::string engineAssociation;

    [[nodiscard]] bool hasEngineBinding() const noexcept {
        return !engineVersion.empty() || !engineHash.empty();
    }
    [[nodiscard]] bool hasEngineAssociation() const noexcept {
        return !engineAssociation.empty();
    }
};

// Files generated for an individual project.  When the linked engine ships its
// MSBuild projects, the solution references those projects so opening the
// project solution also opens and builds the engine source together.  The
// build metadata records the exact engine link (association/version/hash), so
// a project can be distributed independently from the engine checkout.
struct ProjectBuildFiles {
    std::filesystem::path solution;
    std::filesystem::path buildManifest;
};

// Just the manifest identity/link fields, read without resolving or validating
// the Project Manager uses this to recover a project whose engineRoot
// is missing or stale by resolving `engineAssociation` through the registry.
struct ProjectManifestInfo {
    std::filesystem::path manifest;
    std::filesystem::path projectRoot;
    std::string name;
    std::string engineAssociation;
    std::string engineVersion;
    std::string engineHash;
};

// A per-user registry of installed engines, keyed by a named association.  The
// Project Manager resolves a project's `engineAssociation` through this list when a
// project's cached engineRoot is missing or the user switches engine versions.
struct EngineRegistryEntry {
    std::string association;
    std::filesystem::path root;
};
struct EngineRegistry {
    std::vector<EngineRegistryEntry> engines;
};

// Per-user registry location (LocalAppData on Windows, XDG config elsewhere).
[[nodiscard]] std::filesystem::path defaultEngineRegistryPath();
bool loadEngineRegistry(const std::filesystem::path& path, EngineRegistry& result, std::string& error);
bool saveEngineRegistry(const std::filesystem::path& path, const EngineRegistry& registry, std::string& error);
// Replace any entry with the same association or root, then append this one.
void registerEngineInRegistry(EngineRegistry& registry, std::string_view association, std::filesystem::path root);
bool findEngineByAssociation(const EngineRegistry& registry, std::string_view association, EngineRegistryEntry& result);

// Reads Engine.raeengine from an engine root.  A missing manifest is an error
// here; loadProject intentionally treats it as a legacy engine instead.
bool loadEngineIdentity(const std::filesystem::path& engineRoot, EngineIdentity& result, std::string& error);

// Compares the project binding with the engine discovered at project.engine.
EngineCompatibilityReport checkEngineCompatibility(const ProjectPaths& project);
// Compare against a candidate engine when the project manager offers a switch.
EngineCompatibilityReport checkEngineCompatibility(const ProjectPaths& project, const EngineIdentity& candidate);

// Helpers used by the project manager when creating or upgrading installations.
bool writeEngineManifest(const std::filesystem::path& engineRoot, std::string_view name,
                         std::string_view version, std::string_view hash, std::string& error);
bool updateProjectEngineBinding(const std::filesystem::path& folderOrManifest,
                                const EngineIdentity& engine, std::string& error);

// Rebinds a project whose cached engineRoot is missing or stale, then reloads
// it. Prefers the running engine (preferredEngineRoot) and falls back to the
// engine registered for the manifest's association. Only an engine whose
// version matches the recorded binding is used; switching versions stays an
// explicit project-manager action. The rewritten manifest stores engineRoot
// relative to the manifest (or only the association when it lives on another
// volume), so a moved project keeps working on another machine.
bool recoverProject(const std::filesystem::path& folderOrManifest,
                    const std::filesystem::path& preferredEngineRoot,
                    ProjectPaths& result, std::string& error);

// Create or refresh the project-local solution and build metadata.  This is
// safe to call after an engine binding update and is idempotent.  It does not
// copy engine sources or binaries into the project directory.
// Note: the current data-only project manager does NOT auto-invoke this; it is
// retained for future projects that ship native C++ modules.
bool writeProjectBuildFiles(const ProjectPaths& project, ProjectBuildFiles& result,
                            std::string& error);

// Accepts a .raproject manifest or a directory containing exactly one manifest.
// All project paths are absolute. Manifest strings (including paths/name) are UTF-8.
// Failure leaves result unchanged.
bool loadProject(const std::filesystem::path& folderOrManifest, ProjectPaths& result, std::string& error);

// Atomically updates only the rendering settings in an existing manifest.
// Unknown project/rendering fields are preserved. Failure retains the original file.
bool saveProjectRenderingSettings(const std::filesystem::path& folderOrManifest,
                                  const RenderingSettings& settings, std::string& error);

// Reads only the manifest identity/link fields (name, association, version,
// hash) without resolving or validating engineRoot. Returns false for a broken
// manifest. Used by the Project Manager to recover a project whose engineRoot is
// missing or stale by resolving engineAssociation through the registry.
bool loadProjectManifestInfo(const std::filesystem::path& folderOrManifest, ProjectManifestInfo& result, std::string& error);
} // namespace rae::project
