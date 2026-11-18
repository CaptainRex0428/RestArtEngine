#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <RestArtEngine/Editor/LoadingScreen.h>
#include "../Renderer/D3D12/D3D12Rhi.h"
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace rae::editor {
namespace {
using Microsoft::WRL::ComPtr;

std::string pathText(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

struct DecodedImage {
    std::uint32_t width = 0, height = 0;
    std::vector<std::byte> pixels;
};

// WIC decoding is shared by the two uploads; it never creates a graphics device.
bool decodeImage(const std::filesystem::path& image, DecodedImage& result, std::string& error) {
    if (!std::filesystem::is_regular_file(image)) {
        error = "Loading image not found: " + pathText(image);
        return false;
    }
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        error = "Cannot initialize COM for the loading image decoder.";
        return false;
    }
    struct ApartmentScope {
        HRESULT status;
        ~ApartmentScope() { if (SUCCEEDED(status)) ::CoUninitialize(); }
    } apartment{com};
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        error = "Windows Imaging Component is unavailable.";
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(image.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, &decoder))) {
        error = "Cannot decode loading image: " + pathText(image);
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        error = "Loading image has no decodable frame.";
        return false;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
            nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        error = "Cannot convert the loading image to RGBA.";
        return false;
    }
    UINT width = 0, height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0) {
        error = "Loading image has invalid dimensions.";
        return false;
    }
    if (width > std::numeric_limits<UINT>::max() / 4u ||
        height > std::numeric_limits<UINT>::max() / (width * 4u)) {
        error = "Loading image exceeds the decoder buffer limit.";
        return false;
    }
    std::vector<std::byte> pixels(static_cast<std::size_t>(width) * height * 4u);
    if (FAILED(converter->CopyPixels(nullptr, width * 4u, static_cast<UINT>(pixels.size()),
        reinterpret_cast<BYTE*>(pixels.data())))) {
        error = "Cannot read loading image pixels.";
        return false;
    }
    result.width = width;
    result.height = height;
    result.pixels = std::move(pixels);
    return true;
}

bool uploadD3D11Image(ID3D11Device* device, const DecodedImage& image,
    ID3D11ShaderResourceView** result, std::string& error) {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = image.width;
    description.Height = image.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = image.pixels.data();
    initial.SysMemPitch = image.width * 4u;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&description, &initial, &texture))) {
        error = "Cannot create the loading image texture.";
        return false;
    }
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &view))) {
        error = "Cannot create the loading image view.";
        return false;
    }
    *result = view.Detach();
    return true;
}
} // namespace

LoadingScreen::~LoadingScreen() { release(); }

bool LoadingScreen::initialize(ID3D11Device* device, const std::filesystem::path& image, std::string& error) {
    release();
    error.clear();
    if (!device) {
        error = "The loading image requires an initialized D3D11 device.";
        return false;
    }
    DecodedImage decoded;
    if (!decodeImage(image, decoded, error)) return false;
    ID3D11ShaderResourceView* view = nullptr;
    if (!uploadD3D11Image(device, decoded, &view, error)) return false;
    texture_ = view;
    textureId_ = reinterpret_cast<std::uintptr_t>(view);
    width_ = decoded.width;
    height_ = decoded.height;
    return true;
}

bool LoadingScreen::initializeGraphicsDevice(std::shared_ptr<renderer::rhi::Device> graphics,
    const std::filesystem::path& image, std::string& error) {
    release();
    error.clear();
    try {
        auto* native = dynamic_cast<renderer::d3d12::D3D12RhiDevice*>(graphics.get());
        if (!native) {
            error = "The active backend cannot upload a loading image.";
            return false;
        }
        DecodedImage decoded;
        if (!decodeImage(image, decoded, error)) return false;
        auto texture = native->uploadRgba8(decoded.width, decoded.height, decoded.pixels, "Loading artwork");
        if (!texture) throw std::runtime_error("Cannot create the D3D12 loading image texture.");
        const auto textureId = native->uiTextureId(texture);
        if (!textureId) throw std::runtime_error("Cannot create the D3D12 loading image view.");
        textureDevice_ = std::move(graphics);
        nativeTexture_ = std::move(texture);
        textureId_ = textureId;
        width_ = decoded.width;
        height_ = decoded.height;
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

void LoadingScreen::release() noexcept {
    // The boot artwork may still be referenced by the last submitted UI list.
    // Keep its device alive and drain that work before releasing the handle.
    if (nativeTexture_) {
        if (auto* native = dynamic_cast<renderer::d3d12::D3D12RhiDevice*>(textureDevice_.get())) {
            try { native->waitIdle(); } catch (...) { /* Device loss must not throw from teardown. */ }
        }
    }
    nativeTexture_.reset();
    textureDevice_.reset();
    if (texture_) {
        texture_->Release();
        texture_ = nullptr;
    }
    textureId_ = 0;
    width_ = height_ = 0;
}

void LoadingScreen::draw(float progress, const char* text, const char* detail) const {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float displayWidth = viewport->WorkSize.x, displayHeight = viewport->WorkSize.y;
    if (displayWidth < 1.0f || displayHeight < 1.0f) return;
    ImDrawList* list = ImGui::GetBackgroundDrawList(viewport);
    const ImVec2 topLeft = viewport->WorkPos;
    const ImVec2 bottomRight(topLeft.x + displayWidth, topLeft.y + displayHeight);
    const auto position = [topLeft](float x, float y) { return ImVec2(topLeft.x + x, topLeft.y + y); };
    list->PushClipRect(topLeft, bottomRight, true);
    if (textureId_)
        list->AddImage(static_cast<ImTextureID>(textureId_), topLeft, bottomRight);
    else
        list->AddRectFilled(topLeft, bottomRight, IM_COL32(10, 12, 18, 255));
    // A soft scrim keeps the overlay readable on bright artwork.
    list->AddRectFilledMultiColor(position(0.0f, displayHeight * 0.72f), bottomRight,
        IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 152), IM_COL32(0, 0, 0, 152));

    // Every overlay coordinate is a fraction of the artwork size, so replacing
    // the background image keeps the same composition.
    const float inset = displayWidth * 0.055f;
    const float right = displayWidth - inset;
    const float fontSize = std::clamp(displayHeight * 0.021f, 13.0f, 26.0f);
    const float detailSize = std::max(11.0f, fontSize * .85f);
    const float detailTop = displayHeight * .921f - detailSize - 6;
    const float textTop = std::min(displayHeight * .825f, detailTop - fontSize - 4);
    ImFont* font = ImGui::GetFont();
    const char* label = (text && *text) ? text : "Loading";

    const float clamped = std::clamp(progress, 0.0f, 1.0f);
    char percent[16];
    std::snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(clamped * 100.0f + 0.5f));
    const ImVec2 percentSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, percent);
    // Reserve the percentage column, including on the small borderless splash.
    const ImVec4 labelClip(topLeft.x + inset, topLeft.y + textTop,
        topLeft.x + right - percentSize.x - fontSize, topLeft.y + textTop + fontSize + 2);
    list->AddText(font, fontSize, position(inset, textTop), IM_COL32(228, 238, 248, 255),
        label, nullptr, 0.0f, &labelClip);
    list->AddText(font, fontSize, position(right - percentSize.x, textTop), IM_COL32(150, 205, 255, 255), percent);

    if (detail && *detail) {
        const float available = right - inset;
        std::string visible(detail);
        const char* ellipsis = "...";
        const float ellipsisWidth = font->CalcTextSizeA(detailSize, FLT_MAX, 0, ellipsis).x;
        if (font->CalcTextSizeA(detailSize, FLT_MAX, 0, visible.c_str()).x > available) {
            // Keep the leading count/status and truncate only at UTF-8 boundaries.
            const char* end = nullptr;
            font->CalcTextSizeA(detailSize, std::max(1.0f, available - ellipsisWidth), 0,
                visible.c_str(), nullptr, &end);
            visible.resize(static_cast<std::size_t>(end - visible.c_str()));
            visible += ellipsis;
        }
        const ImVec4 detailClip(topLeft.x + inset, topLeft.y + detailTop,
            topLeft.x + right, topLeft.y + detailTop + detailSize + 2);
        list->AddText(font, detailSize, position(inset, detailTop), IM_COL32(177, 193, 210, 255),
            visible.c_str(), nullptr, 0, &detailClip);
    }

    // Thin, minimal two-tone bar: light blue for the loaded portion, grey track.
    const float barTop = displayHeight * 0.921f;
    const float barHeight = std::max(3.0f, displayHeight * 0.0042f);
    const float rounding = barHeight * 0.5f;
    list->AddRectFilled(position(inset, barTop), position(right, barTop + barHeight), IM_COL32(126, 132, 142, 120), rounding);
    if (clamped > 0.0f) {
        list->AddRectFilled(position(inset, barTop),
            position(inset + (right - inset) * clamped, barTop + barHeight), IM_COL32(126, 200, 255, 255), rounding);
    }
    list->PopClipRect();
}

} // namespace rae::editor
