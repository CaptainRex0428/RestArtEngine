# Third-party dependencies

The subdirectories in this folder are git submodules. After cloning the
repository, check them out with:

    git submodule update --init

| Path | Upstream | Pinned |
| --- | --- | --- |
| `imgui/` | https://github.com/ocornut/imgui | v1.91.9b |
| `ImGuizmo/` | https://github.com/CedricGuillemet/ImGuizmo | 105e4fde45e4e74aae8973d98750820ef7aca7e2 |
| `nlohmann/` | https://github.com/nlohmann/json | v3.12.0 |

All three are MIT licensed. Provenance notes live in `docs/vendor/`.