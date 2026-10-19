#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace rae::editor {

// Full-screen loading presentation shown while the engine boots. The artwork is
// decoded once into a GPU texture and stretched to the window; the overlay is
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

    // Must be called inside an ImGui frame; draws into the background draw list.
    void draw(float displayWidth, float displayHeight, float progress, const char* text) const;

private:
    ID3D11ShaderResourceView* texture_ = nullptr;
};

} // namespace rae::editor
