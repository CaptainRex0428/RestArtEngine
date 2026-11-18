#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../src/Renderer/D3D11/D3D11Rhi.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
namespace rhi = rae::renderer::rhi;
namespace backend = rae::renderer::d3d11;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void checked(HRESULT result, const char* operation) {
    if (FAILED(result))
        throw std::runtime_error(std::string(operation) + " failed (HRESULT " +
            std::to_string(static_cast<unsigned long>(result)) + ").");
}

template<class Operation>
void rejects(Operation operation, const char* message) {
    try {
        operation();
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error(message);
}

template<class T, std::size_t N>
std::span<const std::byte> bytes(const std::array<T, N>& values) {
    return std::as_bytes(std::span<const T>(values));
}

template<class T>
std::span<const std::byte> bytes(const T& value) {
    return std::as_bytes(std::span<const T>(&value, 1));
}

struct Fixture {
    ComPtr<ID3D11Device> nativeDevice;
    ComPtr<ID3D11DeviceContext> nativeContext;
    std::shared_ptr<rhi::Device> device;

    Fixture() {
        D3D_FEATURE_LEVEL level{};
        const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0};
        checked(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            requested, 1, D3D11_SDK_VERSION, &nativeDevice, &level, &nativeContext),
            "D3D11CreateDevice(WARP)");
        require(level >= D3D_FEATURE_LEVEL_11_0, "WARP must provide feature level 11.0.");
        device = backend::createRhiDevice(nativeDevice.Get(), nativeContext.Get());
        require(static_cast<bool>(device), "D3D11 RHI device creation returned null.");
    }
};

ComPtr<ID3DBlob> compile(const char* entry, const char* profile) {
    constexpr char source[] = R"(
cbuffer DrawConstants : register(b0) { float4 offset; float4 color; };
float4 VSMain(float2 position : POSITION) : SV_POSITION {
    return float4(position + offset.xy, 0.0, 1.0);
}
float4 PSMain() : SV_TARGET { return color; }
)";
    ComPtr<ID3DBlob> code, diagnostics;
    const HRESULT result = D3DCompile(source, sizeof(source) - 1, "RhiTests.hlsl",
        nullptr, nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS |
        D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &code, &diagnostics);
    if (FAILED(result)) {
        const std::string message = diagnostics
            ? std::string(static_cast<const char*>(diagnostics->GetBufferPointer()),
                diagnostics->GetBufferSize()) : "No compiler diagnostic.";
        throw std::runtime_error("GPU test shader compilation failed: " + message);
    }
    return code;
}

std::span<const std::byte> bytecode(ID3DBlob* blob) {
    return {static_cast<const std::byte*>(blob->GetBufferPointer()), blob->GetBufferSize()};
}

using Pixel = std::array<unsigned char, 4>;

// Native access is restricted to readback. All production of the image is RHI work.
std::vector<Pixel> readback(Fixture& fixture, const rhi::TextureHandle& texture) {
    auto* source = backend::nativeTexture(texture);
    require(source != nullptr, "RHI texture has no D3D11 readback source.");
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    require(description.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
        description.ArraySize == 1 && description.SampleDesc.Count == 1,
        "Readback helper expects a single-layer, non-MSAA RGBA8 target.");
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    checked(fixture.nativeDevice->CreateTexture2D(&description, nullptr, &staging),
        "Create readback staging texture");
    fixture.nativeContext->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checked(fixture.nativeContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Map readback staging texture");
    std::vector<Pixel> pixels(description.Width * description.Height);
    for (UINT y = 0; y < description.Height; ++y)
        std::memcpy(pixels.data() + y * description.Width,
            static_cast<const std::byte*>(mapped.pData) + y * mapped.RowPitch,
            description.Width * sizeof(Pixel));
    fixture.nativeContext->Unmap(staging.Get(), 0);
    return pixels;
}

void expectPixel(const std::vector<Pixel>& pixels, unsigned x, unsigned y,
    Pixel expected, const char* message) {
    constexpr unsigned width = 64;
    const auto actual = pixels.at(y * width + x);
    for (unsigned channel = 0; channel < 4; ++channel) {
        const int difference = static_cast<int>(actual[channel]) - expected[channel];
        if (difference < -2 || difference > 2)
            throw std::runtime_error(std::string(message) + " (channel " +
                std::to_string(channel) + ", actual " + std::to_string(actual[channel]) + ").");
    }
}

void runGpuTests() {
    Fixture fixture;
    auto& device = *fixture.device;
    auto& commands = device.commands();
    const auto vsCode = compile("VSMain", "vs_5_0");
    const auto psCode = compile("PSMain", "ps_5_0");
    const auto vs = device.createShader(rhi::ShaderStage::Vertex, bytecode(vsCode.Get()), "Test VS");
    const auto ps = device.createShader(rhi::ShaderStage::Pixel, bytecode(psCode.Get()), "Test PS");

    const std::array<std::array<float, 2>, 3> vertices{{
        {-0.35f, -0.65f}, {0.0f, 0.65f}, {0.35f, -0.65f}}};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    const auto vertex = device.createBuffer({sizeof(vertices), rhi::BufferUsage::Vertex,
        true, "Immutable vertices"}, bytes(vertices));
    const auto index = device.createBuffer({sizeof(indices), rhi::BufferUsage::Index,
        true, "Immutable indices"}, bytes(indices));
    struct Constants { std::array<float, 4> offset, color; };
    static_assert(sizeof(Constants) == 32);
    const auto constants = device.createBuffer({sizeof(Constants), rhi::BufferUsage::Constant,
        false, "Per-draw constants"});
    const auto target = device.createTexture({64, 32, 1, rhi::Format::RGBA8Unorm,
        true, "Readback target"});

    rhi::GraphicsPipelineDesc pipelineDescription;
    pipelineDescription.vertexShader = vs;
    pipelineDescription.pixelShader = ps;
    pipelineDescription.attributes = {{"POSITION", 0, 0, 0, 2}};
    pipelineDescription.depthEnable = false;
    pipelineDescription.depthWrite = false;
    pipelineDescription.colorFormats = {rhi::Format::RGBA8Unorm};
    pipelineDescription.name = "GPU test pipeline";
    const auto pipeline = device.createGraphicsPipeline(pipelineDescription);

    rejects([&] { commands.draw(3); }, "Draw outside a render pass must fail.");
    rejects([&] { commands.drawIndexed(3); }, "Indexed draw outside a pass must fail.");
    rejects([&] { commands.writeBuffer(vertex, bytes(vertices)); },
        "Writes to immutable buffers must fail.");
    const std::array<std::byte, sizeof(Constants) + 16> oversized{};
    rejects([&] { commands.writeBuffer(constants, bytes(oversized)); },
        "A write exceeding the buffer size must fail.");
    rejects([&] { commands.bindIndexBuffer(vertex); }, "A vertex buffer is not an index buffer.");
    const std::array wrongConstants{vertex};
    rejects([&] { commands.bindConstantBuffers(rhi::ShaderStage::Vertex, 0, wrongConstants); },
        "A vertex buffer is not a constant buffer.");
    const std::array wrongVertices{rhi::VertexBufferBinding{constants, 8, 0}};
    rejects([&] { commands.bindVertexBuffers(0, wrongVertices); },
        "A constant buffer is not a vertex buffer.");

    const std::array<std::byte, 8> invalidBytecode{};
    rejects([&] { (void)device.createShader(rhi::ShaderStage::Vertex, bytes(invalidBytecode)); },
        "Invalid shader bytecode must fail.");
    auto invalidPipeline = pipelineDescription;
    invalidPipeline.vertexShader.reset();
    rejects([&] { (void)device.createGraphicsPipeline(invalidPipeline); },
        "A pipeline with no vertex shader must fail.");
    invalidPipeline.vertexShader = ps;
    rejects([&] { (void)device.createGraphicsPipeline(invalidPipeline); },
        "A pixel shader cannot serve as the pipeline vertex shader.");

    rhi::RenderPassDesc pass;
    pass.name = "Clear blue and draw independent constants";
    pass.colors = {{target, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 1, 1}}};
    auto aliased = pass;
    aliased.reads = {{target, 0}};
    rejects([&] { commands.beginRenderPass(aliased); },
        "A pass cannot sample and render to the same texture.");

    Fixture foreign;
    const auto foreignBuffer = foreign.device->createBuffer({sizeof(Constants),
        rhi::BufferUsage::Constant, false, "Foreign constants"});
    const auto foreignTarget = foreign.device->createTexture({64, 32, 1,
        rhi::Format::RGBA8Unorm, true, "Foreign target"});
    rejects([&] { commands.writeBuffer(foreignBuffer, bytes(Constants{})); },
        "A buffer from another device must be rejected.");
    auto foreignPass = pass;
    foreignPass.colors[0].texture = foreignTarget;
    rejects([&] { commands.beginRenderPass(foreignPass); },
        "A render target from another device must be rejected.");
    const auto foreignVs = foreign.device->createShader(rhi::ShaderStage::Vertex,
        bytecode(vsCode.Get()), "Foreign VS");
    invalidPipeline = pipelineDescription;
    invalidPipeline.vertexShader = foreignVs;
    rejects([&] { (void)device.createGraphicsPipeline(invalidPipeline); },
        "A pipeline cannot contain a shader from another device.");

    commands.beginRenderPass(pass);
    rejects([&] { commands.draw(3); }, "Draw without a bound pipeline must fail.");
    auto mismatchedDescription = pipelineDescription;
    mismatchedDescription.colorFormats = {rhi::Format::RGBA16Float};
    const auto mismatchedPipeline = device.createGraphicsPipeline(mismatchedDescription);
    rejects([&] { commands.bindGraphicsPipeline(mismatchedPipeline); },
        "Pipeline formats must match the active attachments.");
    commands.bindGraphicsPipeline(pipeline);
    commands.setViewport({0, 0, 64, 32, 0, 1});
    // D3D11 permits stride zero, for example to broadcast missing UV data.
    const std::array broadcastVertex{rhi::VertexBufferBinding{vertex, 0, 0}};
    commands.bindVertexBuffers(0, broadcastVertex);
    const std::array vertexBinding{rhi::VertexBufferBinding{vertex, 2 * sizeof(float), 0}};
    commands.bindVertexBuffers(0, vertexBinding);
    commands.bindIndexBuffer(index);
    const std::array constantBindings{constants};
    commands.bindConstantBuffers(rhi::ShaderStage::Vertex, 0, constantBindings);
    commands.bindConstantBuffers(rhi::ShaderStage::Pixel, 0, constantBindings);

    const Constants left{{-0.5f, 0, 0, 0}, {1, 0, 0, 1}};
    commands.writeBuffer(constants, bytes(left));
    commands.drawIndexed(3);
    const Constants right{{0.5f, 0, 0, 0}, {0, 1, 0, 1}};
    commands.writeBuffer(constants, bytes(right));
    commands.draw(3);
    commands.endRenderPass();

    auto pixels = readback(fixture, target);
    expectPixel(pixels, 16, 16, {255, 0, 0, 255},
        "First draw must retain red constants after the second upload");
    expectPixel(pixels, 48, 16, {0, 255, 0, 255}, "Second draw must use green constants");
    expectPixel(pixels, 32, 1, {0, 0, 255, 255}, "Clear must initialize background blue");

    pass.name = "Load preserves previous contents";
    pass.colors[0].load = rhi::LoadOp::Load;
    // An intentionally different clear value ensures Load does not perform a clear.
    pass.colors[0].clear = {1, 1, 0, 1};
    commands.beginRenderPass(pass);
    commands.endRenderPass();
    pixels = readback(fixture, target);
    expectPixel(pixels, 16, 16, {255, 0, 0, 255}, "Load must preserve the red triangle");
    expectPixel(pixels, 48, 16, {0, 255, 0, 255}, "Load must preserve the green triangle");
    expectPixel(pixels, 32, 1, {0, 0, 255, 255}, "Load must preserve background");

    pass.name = "Clear replaces previous contents";
    pass.colors[0].load = rhi::LoadOp::Clear;
    commands.beginRenderPass(pass);
    commands.endRenderPass();
    pixels = readback(fixture, target);
    expectPixel(pixels, 16, 16, {255, 255, 0, 255}, "Clear must replace triangle contents");
    expectPixel(pixels, 32, 1, {255, 255, 0, 255}, "Clear must replace background");

    const auto& stats = device.statistics();
    require(stats.buffersCreated >= 3 && stats.texturesCreated >= 1 &&
        stats.shadersCreated >= 2 && stats.pipelinesCreated >= 2,
        "Statistics must account for actual resource creation.");
    require(stats.bufferUploads >= 2 && stats.renderPasses >= 3 &&
        stats.drawCalls >= 1 && stats.indexedDrawCalls >= 1,
        "Statistics must account for actual uploads, passes, and both draw types.");
}
} // namespace

int main() {
    try {
        runGpuTests();
        std::cout << "D3D11 WARP RHI GPU readback and validation tests passed.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
