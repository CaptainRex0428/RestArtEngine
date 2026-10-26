#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <RestArtEngine/Editor/LoadingScreen.h>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace rae::editor {
namespace {
using Microsoft::WRL::ComPtr;

std::string pathText(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

// Decodes the PNG through WIC and uploads it as an immutable shader resource.
bool decodeImage(ID3D11Device* device, const std::filesystem::path& image,
                 ID3D11ShaderResourceView** result, std::string& error) {
    if (!device) { error = "A render device is required to load the loading image."; return false; }
    if (!std::filesystem::is_regular_file(image)) {
        error = "Loading image not found: " + pathText(image);
        return false;
    }
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        error = "Cannot initialize COM for the loading image decoder.";
        return false;
    }
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
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4u);
    if (FAILED(converter->CopyPixels(nullptr, width * 4u, static_cast<UINT>(pixels.size()), pixels.data()))) {
        error = "Cannot read loading image pixels.";
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = width * 4u;
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
    ID3D11ShaderResourceView* view = nullptr;
    if (!decodeImage(device, image, &view, error)) return false;
    texture_ = view;
    return true;
}

void LoadingScreen::release() noexcept {
    if (texture_) {
        texture_->Release();
        texture_ = nullptr;
    }
}

void LoadingScreen::draw(float displayWidth, float displayHeight, float progress, const char* text) const {
    if (displayWidth < 1.0f || displayHeight < 1.0f) return;
    ImGuiViewport* const viewport = ImGui::GetMainViewport();
    ImDrawList* list = ImGui::GetBackgroundDrawList(viewport);
    const ImVec2 topLeft = viewport->Pos;
    const ImVec2 bottomRight(topLeft.x + displayWidth, topLeft.y + displayHeight);
    const auto position = [topLeft](float x, float y) { return ImVec2(topLeft.x + x, topLeft.y + y); };
    if (texture_)
        list->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture_)), topLeft, bottomRight);
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
    const float textTop = displayHeight * 0.872f;
    ImFont* font = ImGui::GetFont();
    const char* label = (text && *text) ? text : "Loading";
    list->AddText(font, fontSize, position(inset, textTop), IM_COL32(228, 238, 248, 255), label);

    const float clamped = std::clamp(progress, 0.0f, 1.0f);
    char percent[16];
    std::snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(clamped * 100.0f + 0.5f));
    const ImVec2 percentSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, percent);
    list->AddText(font, fontSize, position(right - percentSize.x, textTop), IM_COL32(150, 205, 255, 255), percent);

    // Thin, minimal two-tone bar: light blue for the loaded portion, grey track.
    const float barTop = displayHeight * 0.921f;
    const float barHeight = std::max(3.0f, displayHeight * 0.0042f);
    const float rounding = barHeight * 0.5f;
    list->AddRectFilled(position(inset, barTop), position(right, barTop + barHeight), IM_COL32(126, 132, 142, 120), rounding);
    if (clamped > 0.0f) {
        list->AddRectFilled(position(inset, barTop),
            position(inset + (right - inset) * clamped, barTop + barHeight), IM_COL32(126, 200, 255, 255), rounding);
    }
}

} // namespace rae::editor
