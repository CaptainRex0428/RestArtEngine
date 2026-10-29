#include <RestArtEngine/ImGui/ImGuiLayer.h>
#include <RestArtEngine/ImGui/EditorTheme.h>
#include <RestArtEngine/ImGui/WindowChrome.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <cmath>
#include <iostream>
#include <string>
#include <array>
#include <cfloat>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shell32.lib")

// The backend intentionally leaves this declaration to its consumer.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace rae::imgui {
namespace {
template<class T>
std::uint64_t capacityBytes(const ImVector<T>& values) {
    return static_cast<std::uint64_t>(values.Capacity) * sizeof(T);
}

std::uint64_t splitterBytes(const ImDrawListSplitter& splitter) {
    std::uint64_t bytes = capacityBytes(splitter._Channels);
    for (int i = 0; i < splitter._Channels.Size; ++i) {
        if (i == splitter._Current) continue;
        const auto& channel = splitter._Channels[i];
        bytes += capacityBytes(channel._CmdBuffer) + capacityBytes(channel._IdxBuffer);
    }
    return bytes;
}

// Exactly the storage released by the vendored window compaction routine;
// selected splitter channel aliases the draw list and must not be counted twice.
std::uint64_t transientBytes(const ImGuiWindow& window) {
    const auto& list = *window.DrawList;
    std::uint64_t bytes = capacityBytes(window.IDStack) + capacityBytes(window.DC.ChildWindows)
        + capacityBytes(window.DC.ItemWidthStack) + capacityBytes(window.DC.TextWrapPosStack)
        + capacityBytes(list.CmdBuffer) + capacityBytes(list.IdxBuffer) + capacityBytes(list.VtxBuffer)
        + capacityBytes(list._ClipRectStack) + capacityBytes(list._TextureIdStack)
        + capacityBytes(list._CallbacksDataBuf) + capacityBytes(list._Path)
        + splitterBytes(list._Splitter);
    return bytes;
}

std::uint64_t atlasBytes(const ImFontAtlas& atlas) {
    const auto pixels = static_cast<std::uint64_t>(atlas.TexWidth) * atlas.TexHeight;
    return pixels * ((atlas.TexPixelsAlpha8 ? 1u : 0u) + (atlas.TexPixelsRGBA32 ? 4u : 0u));
}

// Private property on engine-owned HWNDs. Integer atoms do not allocate from
// the session-wide string atom table (SetProp with a string can fail with
// ERROR_NOT_ENOUGH_MEMORY when that table is exhausted).
const ATOM contextAtom = GlobalAddAtomW(MAKEINTATOM(0x5241));
const wchar_t* const contextProperty = MAKEINTATOM(contextAtom);
const ATOM platformProcedureAtom = GlobalAddAtomW(MAKEINTATOM(0x5242));
const wchar_t* const platformProcedureProperty = MAKEINTATOM(platformProcedureAtom);
const ATOM platformContextAtom = GlobalAddAtomW(MAKEINTATOM(0x5243));
const wchar_t* const platformContextProperty = MAKEINTATOM(platformContextAtom);

struct PlatformWindowBinding {
    ImGuiContext* context;
    WNDPROC procedure;
    ImGuiLayer* layer;
};
// A native property is normally sufficient. Keep the process-local binding as
// a fallback if Win32 cannot allocate even an integer-keyed property; a visible
// secondary must never be left with no input/context routing.
std::unordered_map<HWND, PlatformWindowBinding> platformWindowBindings;
bool backendContextBindingWarningReported = false;

// The upstream platform callback can run between frames when the engine has
// restored another (or no) current context. Bind the context stored by Win32's
// backend for the entire native dispatch, including its default procedure.
LRESULT CALLBACK platformWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    const auto binding = platformWindowBindings.find(window);
    auto original = reinterpret_cast<WNDPROC>(GetPropW(window, platformProcedureProperty));
    auto* owned = static_cast<ImGuiContext*>(GetPropW(window, platformContextProperty));
    if (binding != platformWindowBindings.end()) {
        if (!original) original = binding->second.procedure;
        if (!owned) owned = binding->second.context;
    }
    auto* backendContext = static_cast<ImGuiContext*>(GetPropA(window, "IMGUI_CONTEXT"));
    if (!owned) owned = backendContext;
    auto* previous = ImGui::GetCurrentContext();
    if (owned) ImGui::SetCurrentContext(owned);
    LRESULT result = 0;
    if (message == WM_DROPFILES && owned && ImGuiLayer::handleMessage(window, message, wparam, lparam)) {
        // The editor will check this drop against the current Content region.
    } else if (message == WM_CLOSE && owned) {
        // Closing a material window only hides its UI. Let ImGui update the
        // p_open flag next frame, then retire both HWND and swap chain together.
        // In this pinned backend DefWindowProc would destroy the HWND too soon.
        if (auto* viewport = ImGui::FindViewportByPlatformHandle(window))
            viewport->PlatformRequestClose = true;
    } else if (owned && backendContext != owned) {
        // The vendored platform procedure returns DefWindowProc immediately
        // when its unchecked string property allocation failed. Its ordinary
        // input handler can instead use our correctly bound current context.
        result = ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
        if (!result) {
            if (auto* viewport = ImGui::FindViewportByPlatformHandle(window)) {
                if (message == WM_MOVE) viewport->PlatformRequestMove = true;
                if (message == WM_SIZE) viewport->PlatformRequestResize = true;
                if (message == WM_MOUSEACTIVATE && (viewport->Flags & ImGuiViewportFlags_NoFocusOnClick))
                    result = MA_NOACTIVATE;
                if (message == WM_NCHITTEST && (viewport->Flags & ImGuiViewportFlags_NoInputs))
                    result = HTTRANSPARENT;
            }
            if (!result) result = DefWindowProcW(window, message, wparam, lparam);
        }
    } else {
        result = original ? CallWindowProcW(original, window, message, wparam, lparam)
                          : DefWindowProcW(window, message, wparam, lparam);
    }
    if (message == WM_NCDESTROY) {
        RemovePropW(window, platformProcedureProperty);
        RemovePropW(window, platformContextProperty);
        platformWindowBindings.erase(window);
    }
    ImGui::SetCurrentContext(previous);
    return result;
}

// Decode the original logo embedded in the executable, independent of cwd and
// project assets. The layer owns the resulting SRV for its DX11 backend lifetime.
HRESULT loadLogo(ID3D11Device* device, ID3D11ShaderResourceView** output) {
    const auto module = GetModuleHandleW(nullptr);
    const auto resource = FindResourceW(module, L"RAENGINE_LOGO", RT_RCDATA);
    if (!resource) return HRESULT_FROM_WIN32(GetLastError());
    const auto loaded = LoadResource(module, resource);
    auto* bytes = static_cast<BYTE*>(LockResource(loaded));
    const auto length = SizeofResource(module, resource);
    if (!bytes || !length) return E_FAIL;

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return initialized;
    struct ComScope {
        bool owned;
        ~ComScope() { if (owned) CoUninitialize(); }
    } com{SUCCEEDED(initialized)};
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    ComPtr<IWICStream> stream;
    if (FAILED(hr = factory->CreateStream(&stream))) return hr;
    if (FAILED(hr = stream->InitializeFromMemory(bytes, length))) return hr;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(hr = factory->CreateDecoderFromStream(stream.Get(), nullptr,
        WICDecodeMetadataCacheOnLoad, &decoder))) return hr;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(hr = decoder->GetFrame(0, &frame))) return hr;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(hr = factory->CreateBitmapScaler(&scaler))) return hr;
    constexpr UINT side = 128; // Plenty of detail for the small caption at high DPI.
    if (FAILED(hr = scaler->Initialize(frame.Get(), side, side, WICBitmapInterpolationModeFant))) return hr;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(hr = factory->CreateFormatConverter(&converter))) return hr;
    if (FAILED(hr = converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return hr;
    std::array<BYTE, side * side * 4> pixels{};
    if (FAILED(hr = converter->CopyPixels(nullptr, side * 4,
        static_cast<UINT>(pixels.size()), pixels.data()))) return hr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = side;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{pixels.data(), side * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(hr = device->CreateTexture2D(&desc, &data, &texture))) return hr;
    return device->CreateShaderResourceView(texture.Get(), nullptr, output);
}

} // namespace

ImGuiLayer::~ImGuiLayer() { shutdown(); }

bool ImGuiLayer::initialize(void* window, ID3D11Device* device, ID3D11DeviceContext* deviceContext,
                            bool enablePlatformWindows) {
    shutdown();
    if (!window || !device || !deviceContext) { std::cerr << "ImGui: missing window/device/context\n"; return false; }
    const auto hwnd = static_cast<HWND>(window);
    if (!IsWindow(hwnd) || GetPropW(hwnd, contextProperty)) { std::cerr << "ImGui: invalid or already attached window\n"; return false; }
    IMGUI_CHECKVERSION();
    auto* previous = ImGui::GetCurrentContext();
    context_ = ImGui::CreateContext();
    if (!context_) {
        ImGui::SetCurrentContext(previous);
        return false;
    }
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context_));
    window_ = window;
    frameCount_ = 0;
    auto& io = ImGui::GetIO();
    // The engine collector owns scheduling. In this vendored version a negative
    // timer actually compacts immediately (despite the header comment); a very
    // large positive timeout disables the independent automatic pass instead.
    io.ConfigMemoryCompactTimer = FLT_MAX;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    if (enablePlatformWindows) io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    // The editor constrains its dock panels to the main viewport. Only an
    // explicitly separate workspace requests its own native platform window.
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigWindowsResizeFromEdges = true;
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    // Keep the built-in monospace Latin glyphs and merge a locally installed CJK font.
    char windowsFolder[MAX_PATH]{};
    if (GetWindowsDirectoryA(windowsFolder, MAX_PATH)) {
        const std::string fontPath = std::string(windowsFolder) + "/Fonts/msyh.ttc";
        if (GetFileAttributesA(fontPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            ImFontConfig fontConfig;
            fontConfig.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f, &fontConfig, io.Fonts->GetGlyphRangesChineseFull());
        }
    }
    applyTheme(currentTheme());
    platformInitialized_ = ImGui_ImplWin32_Init(window);
    if (platformInitialized_) rendererInitialized_ = ImGui_ImplDX11_Init(device, deviceContext);
    initialized_ = platformInitialized_ && rendererInitialized_;
    if (initialized_ && !SetPropW(hwnd, contextProperty, this)) {
        std::cerr << "ImGui: cannot attach window context, Win32 error " << GetLastError() << '\n';
        initialized_ = false;
    }
    if (initialized_ && enablePlatformWindows) {
        auto& platform = ImGui::GetPlatformIO();
        createPlatformWindow_ = platform.Platform_CreateWindow;
        platform.Platform_CreateWindow = createPlatformWindow;
    }
    if (!platformInitialized_ || !rendererInitialized_)
        std::cerr << "ImGui: backend initialization failed (platform=" << platformInitialized_
            << ", renderer=" << rendererInitialized_ << ")\n";
    if (initialized_ && FAILED(loadLogo(device, &logo_)))
        std::cerr << "ImGui: cannot load embedded engine logo\n";
    if (!initialized_) shutdown();
    ImGui::SetCurrentContext(previous);
    return initialized_;
}

void ImGuiLayer::shutdown() noexcept {
    setFileDropHandler({});
    if (context_) {
        auto* owned = static_cast<ImGuiContext*>(context_);
        auto* previous = ImGui::GetCurrentContext();
        if (previous == owned)
            previous = frameActive_ ? static_cast<ImGuiContext*>(previousFrameContext_) : nullptr;
        if (previous == owned) previous = nullptr;
        ImGui::SetCurrentContext(owned);
        if (frameActive_) ImGui::EndFrame();
        // Native windows and their swap chains borrow the backend/device. They
        // must be gone before either backend releases its device references.
        if (platformInitialized_ && rendererInitialized_) ImGui::DestroyPlatformWindows();
        if (rendererInitialized_) ImGui_ImplDX11_Shutdown();
        if (platformInitialized_) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(owned);
        ImGui::SetCurrentContext(previous);
    }
    if (window_ && GetPropW(static_cast<HWND>(window_), contextProperty) == this)
        RemovePropW(static_cast<HWND>(window_), contextProperty);
    createPlatformWindow_ = nullptr;
    context_ = nullptr;
    if (logo_) { logo_->Release(); logo_ = nullptr; }
    previousFrameContext_ = nullptr;
    window_ = nullptr;
    chrome_ = nullptr;
    platformInitialized_ = false;
    rendererInitialized_ = false;
    frameActive_ = false;
    initialized_ = false;
    windowLastUsed_.clear();
    tableLastUsed_.clear();
    tableTempLastUsed_.clear();
}

void ImGuiLayer::beginFrame(float deltaSeconds) noexcept {
    if (!initialized_ || frameActive_) return;
    previousFrameContext_ = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context_));
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::GetIO().DeltaTime = std::isfinite(deltaSeconds) && deltaSeconds > 0
        ? deltaSeconds : (1.0F / 60.0F);
    ImGui::NewFrame();
    if (chrome_) chrome_->draw(window_, logo_);
    frameActive_ = true;
}

void ImGuiLayer::endFrame(bool renderMain) noexcept {
    if (!initialized_ || !frameActive_) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context_));
    ImGui::Render();
    if (renderMain) ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
    const auto now = std::chrono::steady_clock::now();
    const auto& owned = *static_cast<ImGuiContext*>(context_);
    for (const auto* window : owned.Windows) {
        const auto entry = windowLastUsed_.try_emplace(window->ID, now).first;
        if (window->Active) entry->second = now;
    }
    for (int i = 0; i < owned.TablesLastTimeActive.Size; ++i) {
        if (owned.TablesLastTimeActive[i] < 0) continue;
        const auto* table = &owned.Tables.Buf[i];
        const auto entry = tableLastUsed_.try_emplace(table->ID, now).first;
        if (table->LastFrameActive == owned.FrameCount) entry->second = now;
    }
    for (int i = 0; i < owned.TablesTempData.Size; ++i) {
        const auto& temp = owned.TablesTempData[i];
        if (temp.LastTimeActive < 0) continue;
        const auto entry = tableTempLastUsed_.try_emplace(i, now).first;
        if (temp.LastTimeActive == static_cast<float>(owned.Time)) entry->second = now;
    }
    ++frameCount_;
    frameActive_ = false;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(previousFrameContext_));
    previousFrameContext_ = nullptr;
}

bool ImGuiLayer::hasVisiblePlatformWindows() const noexcept {
    if (!context_ || !initialized_) return false;
    const auto& viewports = static_cast<const ImGuiContext*>(context_)->Viewports;
    for (int index = 1; index < viewports.Size; ++index) {
        const auto* viewport = viewports[index];
        const auto hwnd = static_cast<HWND>(viewport->PlatformHandleRaw
            ? viewport->PlatformHandleRaw : viewport->PlatformHandle);
        if (hwnd && IsWindowVisible(hwnd) && !IsIconic(hwnd)) return true;
    }
    return false;
}

bool ImGuiLayer::hasPendingPlatformWindowRequests() const noexcept {
    if (!context_ || !initialized_) return false;
    const auto& viewports = static_cast<const ImGuiContext*>(context_)->Viewports;
    for (int index = 1; index < viewports.Size; ++index) {
        const auto* viewport = viewports[index];
        if (viewport->PlatformRequestClose || viewport->PlatformRequestMove || viewport->PlatformRequestResize)
            return true;
    }
    return false;
}

void ImGuiLayer::createPlatformWindow(ImGuiViewport* viewport) {
    const auto* mainViewport = ImGui::GetMainViewport();
    const auto mainWindow = static_cast<HWND>(mainViewport->PlatformHandleRaw
        ? mainViewport->PlatformHandleRaw : mainViewport->PlatformHandle);
    auto* layer = static_cast<ImGuiLayer*>(GetPropW(mainWindow, contextProperty));
    if (!layer || !layer->createPlatformWindow_) return;
    layer->createPlatformWindow_(viewport);
    const auto hwnd = static_cast<HWND>(viewport->PlatformHandleRaw
        ? viewport->PlatformHandleRaw : viewport->PlatformHandle);
    if (!hwnd) return;
    const auto original = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    auto* owned = ImGui::GetCurrentContext();
    platformWindowBindings.insert_or_assign(hwnd, PlatformWindowBinding{owned, original, layer});
    if (!SetPropW(hwnd, platformProcedureProperty, reinterpret_cast<HANDLE>(original))) {
        std::cerr << "ImGui: native procedure property failed; retaining the engine registry binding.\n";
    }
    if (!SetPropW(hwnd, platformContextProperty, owned)) {
        std::cerr << "ImGui: native context property failed; retaining the engine registry binding.\n";
    }
    SetLastError(0);
    if (!SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(platformWindowProc)) && GetLastError()) {
        RemovePropW(hwnd, platformProcedureProperty);
        RemovePropW(hwnd, platformContextProperty);
        platformWindowBindings.erase(hwnd);
        std::cerr << "ImGui: cannot subclass native window, Win32 error " << GetLastError() << '\n';
    } else if (GetPropA(hwnd, "IMGUI_CONTEXT") != owned && !backendContextBindingWarningReported) {
        backendContextBindingWarningReported = true;
        std::cerr << "ImGui: native backend context property unavailable; using the engine binding.\n";
    }
    DragAcceptFiles(hwnd, static_cast<BOOL>(static_cast<bool>(layer->fileDropHandler_)));
    // Reuse the main window's embedded application icon without owning a second
    // icon resource lifetime. The OS/backend owns the secondary window itself.
    for (const auto size : {ICON_BIG, ICON_SMALL}) {
        const auto icon = SendMessageW(mainWindow, WM_GETICON, size, 0);
        if (icon) SendMessageW(hwnd, WM_SETICON, size, icon);
    }
}

ImGuiMemoryStats ImGuiLayer::memoryStats() const {
    ImGuiMemoryStats result;
    if (!context_) return result;
    const auto& context = *static_cast<ImGuiContext*>(context_);
    if (context.IO.Fonts) result.fontAtlasCpuBytes = atlasBytes(*context.IO.Fonts);
    for (const auto* window : context.Windows) result.windowTransientCpuBytes += transientBytes(*window);
    for (int i = 0; i < context.TablesLastTimeActive.Size; ++i) {
        if (context.TablesLastTimeActive[i] < 0) continue;
        const auto* table = &context.Tables.Buf[i];
        result.tableTransientCpuBytes += capacityBytes(table->SortSpecsMulti) + capacityBytes(table->ColumnsNames.Buf);
    }
    for (const auto& temp : context.TablesTempData) result.tableTransientCpuBytes += splitterBytes(temp.DrawSplitter);
    return result;
}

rae::GarbageCollectionMemoryStats ImGuiLayer::collectGarbage(double maxIdleSeconds) {
    rae::GarbageCollectionMemoryStats result;
    if (!initialized_ || frameActive_ || !std::isfinite(maxIdleSeconds) || maxIdleSeconds < 0) return result;
    auto* owned = static_cast<ImGuiContext*>(context_);
    if (owned->WithinFrameScope) return result;
    auto* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(owned);
    auto* atlas = owned->IO.Fonts;
    // Upload precedes the first completed UI frame. Retain input font data and
    // glyph/UV metadata so a future DX11 device-object rebuild can regenerate it.
    if (frameCount_ > 0 && atlas && !atlas->Locked && atlas->TexID != 0) {
        result.cpuBytesReleased += atlasBytes(*atlas);
        result.objectsReleased += (atlas->TexPixelsAlpha8 ? 1u : 0u) + (atlas->TexPixelsRGBA32 ? 1u : 0u);
        atlas->ClearTexData();
    }
    const auto now = std::chrono::steady_clock::now();
    for (auto* window : owned->Windows) {
        if (window->Active || window->MemoryCompacted) continue;
        const auto used = windowLastUsed_.find(window->ID);
        if (used == windowLastUsed_.end() ||
            std::chrono::duration<double>(now - used->second).count() < maxIdleSeconds) continue;
        const auto bytes = transientBytes(*window);
        ImGui::GcCompactTransientWindowBuffers(window);
        result.cpuBytesReleased += bytes;
        if (bytes) ++result.objectsReleased;
    }
    for (int i = 0; i < owned->TablesLastTimeActive.Size; ++i) {
        if (owned->TablesLastTimeActive[i] < 0) continue;
        auto* table = owned->Tables.GetByIndex(i);
        if (table->MemoryCompacted || table->LastFrameActive == owned->FrameCount) continue;
        const auto used = tableLastUsed_.find(table->ID);
        if (used == tableLastUsed_.end() ||
            std::chrono::duration<double>(now - used->second).count() < maxIdleSeconds) continue;
        const auto bytes = capacityBytes(table->SortSpecsMulti) + capacityBytes(table->ColumnsNames.Buf);
        ImGui::TableGcCompactTransientBuffers(table);
        result.cpuBytesReleased += bytes;
        if (bytes) ++result.objectsReleased;
    }
    for (int i = 0; i < owned->TablesTempData.Size; ++i) {
        auto& temp = owned->TablesTempData[i];
        if (temp.LastTimeActive < 0 || temp.LastTimeActive == static_cast<float>(owned->Time)) continue;
        const auto used = tableTempLastUsed_.find(i);
        if (used == tableTempLastUsed_.end() ||
            std::chrono::duration<double>(now - used->second).count() < maxIdleSeconds) continue;
        const auto bytes = splitterBytes(temp.DrawSplitter);
        ImGui::TableGcCompactTransientBuffers(&temp);
        result.cpuBytesReleased += bytes;
        if (bytes) ++result.objectsReleased;
    }
    ImGui::SetCurrentContext(previous);
    return result;
}

void ImGuiLayer::setIniFilename(const char* filename) noexcept {
    if (!context_) return;
    auto* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context_));
    ImGui::GetIO().IniFilename = filename;
    ImGui::SetCurrentContext(previous);
}

bool ImGuiLayer::handleMessage(void* window, unsigned message,
                               std::uintptr_t wparam, std::intptr_t lparam) {
    if (!window) return false;
    auto* layer = static_cast<ImGuiLayer*>(GetPropW(static_cast<HWND>(window), contextProperty));
    if (!layer) {
        const auto binding = platformWindowBindings.find(static_cast<HWND>(window));
        if (binding != platformWindowBindings.end()) layer = binding->second.layer;
    }
    if (!layer || !layer->initialized_) return false;
    auto* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(layer->context_));
    const auto result = ImGui_ImplWin32_WndProcHandler(
        static_cast<HWND>(window), message, static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
    if (message == WM_DROPFILES) layer->dispatchFileDrop(window, wparam);
    ImGui::SetCurrentContext(previous);
    return message == WM_DROPFILES || result != 0;
}

void ImGuiLayer::setFileDropHandler(FileDropHandler handler) {
    fileDropHandler_ = std::move(handler);
    const BOOL enabled = static_cast<BOOL>(static_cast<bool>(fileDropHandler_));
    if (window_) DragAcceptFiles(static_cast<HWND>(window_), enabled);
    if (context_) {
        const auto& viewports = static_cast<ImGuiContext*>(context_)->Viewports;
        for (int index = 1; index < viewports.Size; ++index) {
            const auto* viewport = viewports[index];
            const auto hwnd = static_cast<HWND>(viewport->PlatformHandleRaw
                ? viewport->PlatformHandleRaw : viewport->PlatformHandle);
            if (hwnd) DragAcceptFiles(hwnd, enabled);
        }
    }
}

void ImGuiLayer::dispatchFileDrop(void* window, std::uintptr_t drop) {
    const auto handle = reinterpret_cast<HDROP>(drop);
    struct FinishDrop { HDROP handle; ~FinishDrop() { DragFinish(handle); } } finish{handle};
    if (!fileDropHandler_) return;
    try {
        std::vector<std::filesystem::path> paths;
        const auto count = DragQueryFileW(handle, 0xffffffffu, nullptr, 0);
        paths.reserve(count);
        for (UINT index = 0; index < count; ++index) {
            const auto length = DragQueryFileW(handle, index, nullptr, 0);
            if (!length) continue;
            std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
            if (!DragQueryFileW(handle, index, path.data(), length + 1)) continue;
            path.resize(length);
            paths.emplace_back(path);
        }
        POINT point{};
        const bool client = DragQueryPoint(handle, &point) != FALSE;
        if (client && (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
            ClientToScreen(static_cast<HWND>(window), &point);
        auto* sourceViewport = ImGui::FindViewportByPlatformHandle(window);
        if (!sourceViewport) {
            for (auto* viewport : ImGui::GetCurrentContext()->Viewports)
                if (viewport->PlatformHandleRaw == window) { sourceViewport = viewport; break; }
        }
        // A nonclient drop is deliberately outside every editor content region.
        fileDropHandler_(std::move(paths), client ? static_cast<float>(point.x) : -FLT_MAX,
            client ? static_cast<float>(point.y) : -FLT_MAX, sourceViewport ? sourceViewport->ID : 0);
    } catch (const std::exception& error) {
        std::cerr << "ImGui: could not queue native file drop: " << error.what() << '\n';
    }
}

} // namespace rae::imgui
