#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace rae::editor {

// Loading presentation shown while the engine boots. The artwork is decoded
// once into a GPU texture and stretched to the main viewport's work area; the overlay is
// laid out in relative coordinates of that image (fractions of width/height) so
// replacing the artwork keeps the same composition.
class LoadingScreen {
public:
    LoadingScreen() = default;
    ~LoadingScreen();
    LoadingScreen(const LoadingScreen&) = delete;
    LoadingScreen& operator=(const LoadingScreen&) = delete;

    // `image` is normally <engineRoot>/assets/load/load.png.
    bool initialize(ID3D11Device* device, const std::filesystem::path& image, std::string& error);
    void release() noexcept;
    [[nodiscard]] bool ready() const noexcept { return texture_ != nullptr; }
    // Artwork aspect (width / height); 16:9 until the image is decoded.
    [[nodiscard]] float aspect() const noexcept {
        return width_ && height_ ? static_cast<float>(width_) / static_cast<float>(height_) : 16.0f / 9.0f;
    }

    // Call after window chrome has reserved this frame's work area. Coordinates
    // follow the main viewport, including desktop offsets with multi-viewports.
    void draw(float progress, const char* text) const;

private:
    ID3D11ShaderResourceView* texture_ = nullptr;
    std::uint32_t width_ = 0, height_ = 0;
};

} // namespace rae::editor
