# Dear ImGui dependency

Upstream: https://github.com/ocornut/imgui/tree/v1.91.9b

Pinned release: **v1.91.9b**. License: MIT, see `LICENSE.txt`.

This directory contains the unmodified core source files, required headers,
and Win32 / DirectX 11 backend files from that release. The demo intentionally
uses a pinned, vendored dependency so both MSBuild and CMake build offline.

RAEngine-specific styling and context ownership live in
`src/ImGui/ImGuiLayer.cpp`, outside this third-party source directory.
