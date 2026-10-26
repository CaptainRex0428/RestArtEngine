#define NOMINMAX
#include <d3d11.h>
#include <wrl/client.h>
#include <RestArtEngine/Editor/LoadingScreen.h>
#include <imgui.h>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool closeEnough(float a, float b) { return std::abs(a - b) < 0.01f; }

std::vector<ImDrawVert> draw(const rae::editor::LoadingScreen& screen, ImVec2 origin, ImVec2 size) {
    ImGui::GetIO().DisplaySize = size;
    ImGui::NewFrame();
    auto* viewport = ImGui::GetMainViewport();
    viewport->Pos = origin;
    viewport->Size = size;
    viewport->WorkPos = {origin.x, origin.y + 36};
    viewport->WorkSize = {size.x, size.y - 36};
    screen.draw(0.65f, "Compiling shader pipeline");
    const auto* list = ImGui::GetBackgroundDrawList(viewport);
    require(list->VtxBuffer.Size >= 4, "Loading page produced no background.");
    const auto min = viewport->WorkPos;
    const ImVec2 max{min.x + viewport->WorkSize.x, min.y + viewport->WorkSize.y};
    require(closeEnough(list->VtxBuffer[0].pos.x, min.x) && closeEnough(list->VtxBuffer[0].pos.y, min.y) &&
        closeEnough(list->VtxBuffer[2].pos.x, max.x) && closeEnough(list->VtxBuffer[2].pos.y, max.y),
        "Loading background does not fill the work area below the caption.");
    if (screen.ready()) {
        require(closeEnough(list->VtxBuffer[0].uv.x, 0) && closeEnough(list->VtxBuffer[0].uv.y, 0) &&
            closeEnough(list->VtxBuffer[2].uv.x, 1) && closeEnough(list->VtxBuffer[2].uv.y, 1),
            "Loading artwork was cropped.");
    }
    for (const auto& command : list->CmdBuffer) {
        if (!command.ElemCount) continue;
        require(closeEnough(command.ClipRect.x, min.x) && closeEnough(command.ClipRect.y, min.y) &&
            closeEnough(command.ClipRect.z, max.x) && closeEnough(command.ClipRect.w, max.y),
            "Loading content can draw over the caption or outside the work area.");
    }
    std::vector<ImDrawVert> vertices(list->VtxBuffer.begin(), list->VtxBuffer.end());
    for (auto& vertex : vertices) {
        vertex.pos.x -= origin.x;
        vertex.pos.y -= origin.y;
    }
    ImGui::EndFrame();
    return vertices;
}
}

int main(int argc, char** argv) {
    ImGui::CreateContext();
    try {
        require(argc == 2, "Pass the loading artwork path.");
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.Fonts->Build();
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)), "Cannot create test device.");
        rae::editor::LoadingScreen artwork, fallback;
        std::string error;
        require(artwork.initialize(device.Get(), argv[1], error), error.c_str());
        for (const auto* screen : {&artwork, &fallback}) {
            for (const auto size : std::array<ImVec2, 3>{{{1440, 900}, {980, 700}, {480, 360}}}) {
                const auto baseline = draw(*screen, {0, 0}, size);
                for (const auto origin : std::array<ImVec2, 2>{{{137, 91}, {-1520, -220}}}) {
                    const auto moved = draw(*screen, origin, size);
                    require(baseline.size() == moved.size(), "Moving the window clips loading content.");
                    for (std::size_t i = 0; i < baseline.size(); ++i)
                        // ImGui snaps glyphs to whole pixels, including across
                        // zero on a monitor with negative desktop coordinates.
                        require(std::abs(baseline[i].pos.x - moved[i].pos.x) <= 1.01f &&
                            std::abs(baseline[i].pos.y - moved[i].pos.y) <= 1.01f,
                            "Loading text/progress geometry depends on desktop position.");
                }
            }
        }
        ImGui::DestroyContext();
        std::cout << "PASS: artwork and fallback, full UVs, caption exclusion, three sizes, positive/negative desktop origins.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        ImGui::DestroyContext();
        return 1;
    }
}
