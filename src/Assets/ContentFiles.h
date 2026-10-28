#pragma once

#include <algorithm>
#include <filesystem>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace rae::assets::detail {

// Ignore name-surrogate links (including junctions), but permit cloud file
// reparse tags: synchronized/hydrated content remains ordinary project data.
inline bool contentLink(const std::filesystem::path& path) {
    std::error_code code;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, code))) return true;
#ifdef _WIN32
    WIN32_FIND_DATAW data{};
    const auto handle = FindFirstFileW(path.c_str(), &data);
    if (handle != INVALID_HANDLE_VALUE) {
        FindClose(handle);
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && (data.dwReserved0 & 0x20000000u)) return true;
    }
#endif
    return false;
}

inline std::vector<std::filesystem::path> contentFiles(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> result;
    if (!std::filesystem::is_directory(folder) || contentLink(folder)) return result;
    auto iterator = std::filesystem::recursive_directory_iterator(folder,
        std::filesystem::directory_options::skip_permission_denied);
    for (const auto& end = std::filesystem::recursive_directory_iterator(); iterator != end; ++iterator) {
        const auto& entry = *iterator;
        const auto name = entry.path().filename().native();
        if (contentLink(entry.path()) || (!name.empty() && name.front() == '.')) {
            iterator.disable_recursion_pending();
            continue;
        }
        if (entry.is_regular_file()) result.push_back(entry.path());
    }
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace rae::assets::detail
