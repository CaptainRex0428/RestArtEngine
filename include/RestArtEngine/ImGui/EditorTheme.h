#pragma once

#include <filesystem>

struct ImVec4;
namespace rae::assets { enum class AssetType; }

namespace rae::imgui {

// Editor colour themes. Black is the neutral grey/black default; DarkBlue keeps
// the previous palette selectable from the UI menu.
enum class EditorTheme {
    Black = 0,
    DarkBlue = 1,
};
inline constexpr int kEditorThemeCount = 2;

[[nodiscard]] const char* themeName(EditorTheme theme);
// Process-wide selection. The ImGui layer applies it when the context is
// created, so the editor can switch themes at runtime without reloading.
[[nodiscard]] EditorTheme& currentTheme();
void applyTheme(EditorTheme theme);
// Convenience: store the selection and apply it immediately.
void setTheme(EditorTheme theme);

// Semantic Content colours shared by both dark themes. This is presentation
// metadata, independent of material values and selection/hover colours.
[[nodiscard]] ImVec4 assetTypeColor(assets::AssetType type);

// Editor UI preferences persisted next to the layout ini. Theme and the
// viewport selection outline share one file so a single load/save keeps them
// consistent; the outline fields are editor-only and never reach scene data.
struct UiPreferences {
    EditorTheme theme = EditorTheme::Black;
    // ImGuizmo's striped indication of an automatically flipped negative axis.
    bool gizmoNegativeAxisHints = true;
    bool selectionOutline = true;
    // Ring radius in scene-texture pixels.
    float outlineThickness = 2.0f;
    // Reuse the active theme accent instead of the fixed brand orange.
    bool outlineThemeColor = false;
    // Brand orange (#FD7804) sampled from the project artwork; also the default
    // entry in the colour presets.
    float outlineColor[3] = {0.992f, 0.471f, 0.016f};
    // Also outline geometry in front of the selection (symmetric depth test).
    bool outlineFrontObjects = false;
};

[[nodiscard]] UiPreferences& uiPreferences();

// Reads `theme=`, `gizmo_negative_axis_hints=`, `outline=`, `outline_thickness=`, `outline_theme_color=` and
// `outline_color=`. Missing keys keep their defaults. When the file does not
// exist yet, the legacy theme-only file is consulted for the palette.
[[nodiscard]] UiPreferences loadUiPreferences(const std::filesystem::path& path);
void saveUiPreferences(const std::filesystem::path& path, const UiPreferences& preferences);
[[nodiscard]] std::filesystem::path defaultUiPreferencesPath();
[[nodiscard]] std::filesystem::path legacyThemePreferencePath();

// Theme-only helpers used by the startup path and the self-test.
[[nodiscard]] EditorTheme loadThemePreference(const std::filesystem::path& path);
void saveThemePreference(const std::filesystem::path& path, EditorTheme theme);

} // namespace rae::imgui
