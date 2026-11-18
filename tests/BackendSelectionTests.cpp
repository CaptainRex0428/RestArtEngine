#include <RestArtEngine/Renderer/BackendSelection.h>

#include <iostream>
#include <stdexcept>

using namespace rae::renderer;

int main() {
    try {
        const auto require = [](bool condition, const char* message) {
            if (!condition) throw std::runtime_error(message);
        };
        for (bool allowFallback : {false, true}) {
            for (auto preference : {MainRenderBackend::D3D11Dxr, MainRenderBackend::Auto,
                 MainRenderBackend::D3D11, MainRenderBackend::D3D12}) {
                const MainBackendRequest request{preference, allowFallback};
                const auto selected = selectMainBackend(request);
                require(selected.requested == request, "Selection must preserve user intent.");
                require(selected.effective == (preference == MainRenderBackend::D3D12
                    ? MainRenderBackend::D3D12 : MainRenderBackend::D3D11),
                    "Explicit native D3D12 must be attempted; Auto remains on validated D3D11.");
                require(selected.startupAllowed, "All supported requests can attempt their native device.");
                require(!selected.fallback, "A startup plan must not fabricate a D3D12 initialization failure.");
                require(selected.allowD3D12Bridge == (preference == MainRenderBackend::D3D11Dxr),
                    "Strict modes, including fallback, must not create a D3D12 bridge.");
                require(selected.reason && selected.reason[0], "Every decision needs an observable reason.");
                const auto fallback = selectD3D11Fallback(request);
                const bool allowed = preference == MainRenderBackend::D3D12 && allowFallback;
                require(fallback.requested == request && fallback.startupAllowed == allowed &&
                    fallback.fallback == allowed,
                    "A failed D3D12 startup may fall back only with the independent policy enabled.");
                require(fallback.effective == MainRenderBackend::D3D11 && !fallback.allowD3D12Bridge,
                    "Fallback selects strict D3D11 without activating the hybrid DXR path.");
                require(fallback.reason && fallback.reason[0], "Allowed or rejected fallback needs a reason.");
            }
        }
        const auto invalid = selectMainBackend({static_cast<MainRenderBackend>(255), true});
        require(!invalid.startupAllowed && !invalid.allowD3D12Bridge && !invalid.fallback,
            "Invalid backend values fail closed instead of silently selecting Auto.");
        const auto invalidFallback = selectD3D11Fallback({static_cast<MainRenderBackend>(255), true});
        require(!invalidFallback.startupAllowed && !invalidFallback.fallback && !invalidFallback.allowD3D12Bridge,
            "Invalid backend requests cannot enter D3D11 through the failure helper.");
        require(!parseMainRenderBackend("d3d13") && !parseMainRenderBackend(""),
            "Invalid configuration values must be distinguishable from Auto.");
        require(parseMainRenderBackend("dx11") == MainRenderBackend::D3D11 &&
            parseMainRenderBackend("dx12") == MainRenderBackend::D3D12,
            "CLI aliases select the requested backend.");
        require(parseMainRenderBackend("d3d11Dxr") == MainRenderBackend::D3D11Dxr &&
            parseMainRenderBackend("legacyHybrid") == MainRenderBackend::D3D11Dxr,
            "Hybrid mode is explicit and retains its previous spelling as an alias.");
        std::cout << "Backend startup selection matrix passed.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
