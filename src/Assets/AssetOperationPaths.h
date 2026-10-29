#pragma once

#include "RestArtEngine/Assets/AssetFile.h"
#include "AssetSerialization.h"
#include "ContentFiles.h"

namespace rae::assets::detail {

inline std::string operationExtension(const std::filesystem::path& path) {
    auto text = pathUtf8(path.extension());
    for (char& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return text;
}

inline void rejectOperationLinks(const std::filesystem::path& path) {
    std::filesystem::path current;
    for (const auto& component : path) {
        current /= component;
        if (contentLink(current)) throw std::runtime_error("Asset path must not contain linked files or directories.");
    }
}

inline std::filesystem::path checkedContentFile(const std::filesystem::path& contentRoot,
    std::string_view relativePath) {
    if (contentRoot.empty()) throw std::runtime_error("Project Content root is not set.");
    if (relativePath.starts_with("engine://")) throw std::runtime_error("Engine Content is read-only.");
    std::string text(relativePath);
    std::replace(text.begin(), text.end(), '\\', '/');
    if (text.empty() || text.find('\0') != std::string::npos || text.find(':') != std::string::npos)
        throw std::runtime_error("Asset path must name a file relative to Project Content.");
    const auto relative = pathFromUtf8(text);
    if (relative.has_root_path()) throw std::runtime_error("Asset path must be relative to Project Content.");
    for (const auto& component : relative) {
        const auto name = pathUtf8(component);
        if (name.empty() || name.front() == '.' || name.back() == '.' || name.back() == ' ')
            throw std::runtime_error("Asset path contains an invalid or hidden component.");
    }
    const auto result = (std::filesystem::absolute(contentRoot).lexically_normal() / relative).lexically_normal();
    rejectOperationLinks(result);
    if (!std::filesystem::is_regular_file(result)) throw std::runtime_error("Asset file no longer exists or is not a regular file.");
    return result;
}

// Unlike the browser's best-effort scan, dependency checks must not silently
// skip inaccessible content. Hidden metadata is outside the Content mount.
inline std::vector<std::filesystem::path> checkedContentFiles(const std::filesystem::path& contentRoot) {
    const auto root = std::filesystem::absolute(contentRoot).lexically_normal();
    rejectOperationLinks(root);
    std::vector<std::filesystem::path> files;
    for (auto it = std::filesystem::recursive_directory_iterator(root);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        const auto name = pathUtf8(it->path().filename());
        if (!name.empty() && name.front() == '.') { it.disable_recursion_pending(); continue; }
        if (contentLink(it->path()))
            throw std::runtime_error("Cannot verify asset dependencies through linked Content: " + pathUtf8(it->path()));
        if (it->is_regular_file()) files.push_back(it->path());
    }
    return files;
}

inline AssetId contentFileIdentity(const std::filesystem::path& path) {
    const auto extension = operationExtension(path);
    if (extension == ".rasset" || extension == ".raasset") {
        AssetFile file;
        std::string error;
        if (!readAssetFileHeader(path, file, error))
            throw std::runtime_error("Cannot verify asset identity: " + pathUtf8(path) + ": " + error);
        return file.id;
    }
    if (extension == ".ramat") {
        const auto value = readJson(path);
        if (value.at("format") != "RAEngine.Material" || value.at("version") != 1)
            throw std::runtime_error("Cannot verify legacy material identity: " + pathUtf8(path));
        return readId(value.at("material").at("id"));
    }
    return 0;
}

inline void requireUniqueContentIdentity(const std::vector<std::filesystem::path>& files,
    AssetId id) {
    std::size_t count = 0;
    for (const auto& file : files) if (contentFileIdentity(file) == id) ++count;
    if (count != 1)
        throw std::runtime_error("This GUID has duplicate or legacy asset files. Resolve them before changing this asset.");
}

} // namespace rae::assets::detail
