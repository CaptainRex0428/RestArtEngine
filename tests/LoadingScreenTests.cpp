#define NOMINMAX
#include <d3d11.h>
#include <wrl/client.h>
#include <RestArtEngine/Editor/LoadingScreen.h>
#include <RestArtEngine/Core/LoadProgress.h>
#include <imgui.h>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <limits>
#include <string>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool closeEnough(float a, float b) { return std::abs(a - b) < 0.01f; }

std::vector<ImDrawVert> draw(const rae::editor::LoadingScreen& screen, ImVec2 origin, ImVec2 size,
    const char* stage = "Compiling shader pipeline", const char* detail = nullptr) {
    ImGui::GetIO().DisplaySize = size;
    ImGui::NewFrame();
    auto* viewport = ImGui::GetMainViewport();
    viewport->Pos = origin;
    viewport->Size = size;
    viewport->WorkPos = {origin.x, origin.y + 36};
    viewport->WorkSize = {size.x, size.y - 36};
    screen.draw(0.65f, stage, detail);
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
    float labelRight = min.x, percentLeft = max.x;
    for (const auto& vertex : list->VtxBuffer) {
        if (vertex.col == IM_COL32(228, 238, 248, 255)) labelRight = std::max(labelRight, vertex.pos.x);
        if (vertex.col == IM_COL32(150, 205, 255, 255)) percentLeft = std::min(percentLeft, vertex.pos.x);
        if (vertex.col == IM_COL32(177, 193, 210, 255))
            require(vertex.pos.x >= min.x && vertex.pos.x <= max.x &&
                vertex.pos.y < min.y + viewport->WorkSize.y * .921f,
                "Resource detail escapes the loading surface or overlaps the bar.");
    }
    require(labelRight <= percentLeft, "A long loading label overlaps the percentage.");
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
        {
            std::vector<float> progress;
            std::string stage, detail;
            rae::LoadProgress root([&](float value, std::string_view current, std::string_view item) {
                progress.push_back(value); stage = current; detail = item;
            });
            const auto child = root.range(.2f, .8f).range(.25f, .75f);
            child.report(0, "Meshes", "0/2");
            child.report(.5f, "Meshes", "1/2: Cube.rasset");
            child.report(1, "Meshes", "2/2: Sphere.rasset");
            require(progress.size() == 3 && closeEnough(progress[0], .35f) &&
                closeEnough(progress[1], .5f) && closeEnough(progress[2], .65f) &&
                stage == "Meshes" && detail == "2/2: Sphere.rasset", "Nested loading ranges lost progress or text.");
            root.report(-1, "Clamp"); root.report(2, "Clamp");
            root.report(std::numeric_limits<float>::quiet_NaN(), "Clamp");
            require(progress[3] == 0 && progress[4] == 1 && progress[5] == 0,
                "Invalid loading fractions must remain finite and bounded.");
            rae::LoadProgress empty;
            empty.range(0, 1).report(.5f, "No observer");
            require(!empty && !empty.range(0, 1), "An absent observer should stay absent through child ranges.");
        }
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)), "Cannot create test device.");
        rae::editor::LoadingScreen artwork, fallback;
        std::string error;
        require(artwork.initialize(device.Get(), argv[1], error), error.c_str());
        for (const auto* screen : {&artwork, &fallback}) {
            for (const auto size : std::array<ImVec2, 4>{{{1440, 900}, {980, 700}, {480, 360}, {320, 180}}}) {
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
                draw(*screen, {137, 91}, size,
                    "Loading project resources with an intentionally long stage label",
                    "123/456: Content/Meshes/An intentionally long resource name that must fit.rasset");
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
