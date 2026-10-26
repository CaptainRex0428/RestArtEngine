# Dear ImGui dependency

Source: https://github.com/ocornut/imgui

Release: [v1.91.9b-docking](https://github.com/ocornut/imgui/releases/tag/v1.91.9b-docking)

Commit: `4806a1924ff6181180bf5e4b8b79ab4394118875` (git submodule, detached HEAD).

License: MIT. The original license is retained in `vendor/imgui/LICENSE.txt` and included by the CMake install rules.

This is the docking variant of the existing 1.91.9b dependency. It provides `ImGuiConfigFlags_DockingEnable`, `DockSpace`, dock tabs, splitters and docking layout persistence. The core sources and Win32/DX11 backends remain upstream sources without RestArtEngine patches.

The editor enables docking in its ImGui context. Native OS multi-viewport windows are a separate feature and require explicit platform/renderer integration; enabling docking alone does not enable them. As of 2026-10-26, the Win32/DX11 editor explicitly enables platform viewports for its separate material workspace, while its six dock panels remain in the main viewport. Scene (Actor properties) and Details (selected Actor instance information) share a tab group by default. Project Manager and render-only validation retain the single-window default; see `docs/Architecture.md` for lifecycle and coordinate contracts.

The internal `DockBuilder` API is version-sensitive. Keep default-layout construction isolated in the editor docking manager and verify it when updating ImGui.

After cloning or updating the parent repository, initialize the pinned dependencies with:

```powershell
git submodule update --init --recursive
```

Version updates must change the submodule reference and this document together, then run the editor smoke checks against the matching upstream headers and backends.
