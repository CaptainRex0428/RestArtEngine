# Dear ImGui dependency

Upstream: https://github.com/ocornut/imgui

Pinned release: **v1.91.9b** (git submodule). License: MIT, see `vendor/imgui/LICENSE.txt`.

This directory is a git submodule holding the unmodified core source files,
required headers, and Win32 / DirectX 11 backend files from that release.
Run `git submodule update --init` after cloning the repository.

RAEngine-specific styling and context ownership live in
`src/ImGui/ImGuiLayer.cpp`, outside this third-party source directory.