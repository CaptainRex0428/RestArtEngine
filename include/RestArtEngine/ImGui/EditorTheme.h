#pragma once

#include <filesystem>

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

// Small text file (`theme=black`) stored beside the ImGui layout ini.
[[nodiscard]] std::filesystem::path defaultThemePreferencePath();
[[nodiscard]] EditorTheme loadThemePreference(const std::filesystem::path& path);
void saveThemePreference(const std::filesystem::path& path, EditorTheme theme);

} // namespace rae::imgui
