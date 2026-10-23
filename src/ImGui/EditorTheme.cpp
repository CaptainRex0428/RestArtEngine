#include <RestArtEngine/ImGui/EditorTheme.h>
#include <imgui.h>
#include <fstream>

namespace rae::imgui {
namespace {

// Geometry is shared: switching themes only recolours, so panel sizes and
// docking behave identically in both palettes.
void applyMetrics() {
    auto& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(18, 18);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 10);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.IndentSpacing = 20;
    style.ScrollbarSize = 12;
    style.GrabMinSize = 10;
    style.WindowRounding = 9;
    style.ChildRounding = 7;
    style.FrameRounding = 5;
    style.PopupRounding = 7;
    style.ScrollbarRounding = 8;
    style.GrabRounding = 4;
    style.TabRounding = 5;
    style.WindowBorderSize = 1;
    style.ChildBorderSize = 1;
    style.FrameBorderSize = 0;
}

void applyBlack() {
    ImGui::StyleColorsDark();
    applyMetrics();
    auto* c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text] = ImVec4(.88F, .88F, .89F, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(.48F, .48F, .50F, 1);
    c[ImGuiCol_WindowBg] = ImVec4(.105F, .105F, .11F, .98F);
    c[ImGuiCol_ChildBg] = ImVec4(.12F, .12F, .125F, .96F);
    c[ImGuiCol_PopupBg] = ImVec4(.14F, .14F, .145F, .99F);
    c[ImGuiCol_Border] = ImVec4(.27F, .27F, .28F, .55F);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = ImVec4(.17F, .17F, .175F, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(.23F, .23F, .24F, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(.29F, .29F, .30F, 1);
    c[ImGuiCol_TitleBg] = ImVec4(.12F, .12F, .125F, 1);
    c[ImGuiCol_TitleBgActive] = ImVec4(.17F, .17F, .18F, 1);
    c[ImGuiCol_TitleBgCollapsed] = c[ImGuiCol_TitleBg];
    c[ImGuiCol_MenuBarBg] = ImVec4(.14F, .14F, .145F, 1);
    c[ImGuiCol_ScrollbarBg] = ImVec4(.10F, .10F, .105F, .70F);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(.30F, .30F, .31F, 1);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(.38F, .38F, .39F, 1);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(.46F, .46F, .47F, 1);
    c[ImGuiCol_CheckMark] = ImVec4(.82F, .82F, .84F, 1);
    c[ImGuiCol_SliderGrab] = ImVec4(.50F, .50F, .52F, 1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(.66F, .66F, .68F, 1);
    c[ImGuiCol_Button] = ImVec4(.21F, .21F, .22F, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(.29F, .29F, .30F, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(.36F, .36F, .37F, 1);
    c[ImGuiCol_Header] = ImVec4(.18F, .18F, .19F, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(.25F, .25F, .26F, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(.31F, .31F, .32F, 1);
    c[ImGuiCol_Separator] = ImVec4(.28F, .28F, .29F, .75F);
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_ButtonHovered];
    c[ImGuiCol_SeparatorActive] = c[ImGuiCol_ButtonActive];
    c[ImGuiCol_ResizeGrip] = ImVec4(.55F, .55F, .57F, .25F);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(.55F, .55F, .57F, .60F);
    c[ImGuiCol_ResizeGripActive] = ImVec4(.55F, .55F, .57F, .95F);
    c[ImGuiCol_Tab] = ImVec4(.15F, .15F, .155F, 1);
    c[ImGuiCol_TabHovered] = c[ImGuiCol_HeaderHovered];
    c[ImGuiCol_TabSelected] = ImVec4(.25F, .25F, .26F, 1);
    c[ImGuiCol_PlotLines] = ImVec4(.70F, .72F, .75F, 1);
    c[ImGuiCol_PlotHistogram] = ImVec4(.65F, .68F, .70F, 1);
    c[ImGuiCol_TextSelectedBg] = ImVec4(.40F, .40F, .42F, .40F);
}

void applyDarkBlue() {
    ImGui::StyleColorsDark();
    applyMetrics();
    auto* c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text] = ImVec4(.89F, .93F, .98F, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(.46F, .54F, .65F, 1);
    c[ImGuiCol_WindowBg] = ImVec4(.055F, .075F, .11F, .98F);
    c[ImGuiCol_ChildBg] = ImVec4(.07F, .095F, .14F, .96F);
    c[ImGuiCol_PopupBg] = ImVec4(.085F, .11F, .16F, .99F);
    c[ImGuiCol_Border] = ImVec4(.21F, .28F, .38F, .55F);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = ImVec4(.12F, .17F, .24F, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(.18F, .28F, .40F, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(.19F, .36F, .50F, 1);
    c[ImGuiCol_TitleBg] = ImVec4(.065F, .09F, .135F, 1);
    c[ImGuiCol_TitleBgActive] = ImVec4(.10F, .15F, .22F, 1);
    c[ImGuiCol_TitleBgCollapsed] = c[ImGuiCol_TitleBg];
    c[ImGuiCol_MenuBarBg] = ImVec4(.085F, .115F, .17F, 1);
    c[ImGuiCol_ScrollbarBg] = ImVec4(.05F, .07F, .10F, .70F);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(.22F, .30F, .40F, 1);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(.29F, .41F, .54F, 1);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(.32F, .52F, .67F, 1);
    c[ImGuiCol_CheckMark] = ImVec4(.29F, .79F, .95F, 1);
    c[ImGuiCol_SliderGrab] = ImVec4(.24F, .65F, .86F, 1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(.39F, .83F, 1, 1);
    c[ImGuiCol_Button] = ImVec4(.15F, .29F, .41F, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(.19F, .43F, .59F, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(.23F, .56F, .71F, 1);
    c[ImGuiCol_Header] = ImVec4(.13F, .25F, .36F, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(.18F, .36F, .49F, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(.20F, .43F, .59F, 1);
    c[ImGuiCol_Separator] = ImVec4(.22F, .31F, .42F, .75F);
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_ButtonHovered];
    c[ImGuiCol_SeparatorActive] = c[ImGuiCol_ButtonActive];
    c[ImGuiCol_ResizeGrip] = ImVec4(.25F, .65F, .85F, .25F);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(.25F, .65F, .85F, .65F);
    c[ImGuiCol_ResizeGripActive] = ImVec4(.25F, .65F, .85F, .95F);
    c[ImGuiCol_Tab] = ImVec4(.10F, .18F, .27F, 1);
    c[ImGuiCol_TabHovered] = c[ImGuiCol_HeaderHovered];
    c[ImGuiCol_TabSelected] = ImVec4(.17F, .34F, .47F, 1);
    c[ImGuiCol_PlotLines] = ImVec4(.32F, .78F, .94F, 1);
    c[ImGuiCol_PlotHistogram] = ImVec4(.35F, .77F, .73F, 1);
    c[ImGuiCol_TextSelectedBg] = ImVec4(.22F, .55F, .72F, .40F);
}

} // namespace

const char* themeName(EditorTheme theme) {
    return theme == EditorTheme::DarkBlue ? "Dark Blue" : "Black";
}

EditorTheme& currentTheme() {
    static EditorTheme theme = EditorTheme::Black;
    return theme;
}

void applyTheme(EditorTheme theme) {
    if (theme == EditorTheme::DarkBlue) applyDarkBlue();
    else applyBlack();
}

void setTheme(EditorTheme theme) {
    currentTheme() = theme;
    applyTheme(theme);
}

std::filesystem::path defaultThemePreferencePath() {
    // Stored next to the ImGui layout ini; both are per-user, never committed.
    return std::filesystem::path("RestArtEngine.theme.ini");
}

EditorTheme loadThemePreference(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) return EditorTheme::Black;
    std::string line;
    while (std::getline(stream, line)) {
        if (line == "theme=darkblue" || line == "theme=dark_blue") return EditorTheme::DarkBlue;
        if (line == "theme=black") return EditorTheme::Black;
    }
    return EditorTheme::Black;
}

void saveThemePreference(const std::filesystem::path& path, EditorTheme theme) {
    std::ofstream stream(path, std::ios::trunc);
    if (!stream) return;
    stream << "theme=" << (theme == EditorTheme::DarkBlue ? "darkblue" : "black") << '\n';
}

} // namespace rae::imgui
