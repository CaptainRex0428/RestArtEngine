#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rae::renderer::captureArchive {

// Portable snapshot storage, not a GPU command stream or a replay format.
// The caller owns the JSON schema and its semantic validation. The container
// validates UTF-8, lengths, version and checksums before publishing a load.
struct Archive {
    std::string metadataJson;
    std::vector<std::vector<std::uint8_t>> payloads;
};

inline constexpr std::uint64_t MaxMetadataBytes = 16ull * 1024 * 1024;
inline constexpr std::uint64_t MaxPayloadBytes = 256ull * 1024 * 1024;
inline constexpr std::uint32_t MaxPayloadCount = 4096;

// Writes a unique adjacent temporary file and atomically replaces the target
// only after a complete write/flush. Failure does not remove a prior target.
[[nodiscard]] bool save(const std::filesystem::path& path, const Archive& archive, std::string& error);
// Failure preserves result. Payloads are opaque bytes and never filesystem
// paths; loading does not create files, instantiate resources or execute code.
[[nodiscard]] bool load(const std::filesystem::path& path, Archive& result, std::string& error);

} // namespace rae::renderer::captureArchive
