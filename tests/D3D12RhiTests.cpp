#include "../src/Renderer/D3D12/D3D12Rhi.h"

#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>

namespace {
using Microsoft::WRL::ComPtr;
namespace rhi = rae::renderer::rhi;
namespace backend = rae::renderer::d3d12;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Operation>
void rejects(Operation operation, const char* message) {
    try { operation(); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error(message);
}
template<class T>
std::span<const std::byte> bytes(const T& value) {
    return std::as_bytes(std::span<const T>(&value, 1));
}
ComPtr<ID3DBlob> compile(const char* entry, const char* profile) {
    constexpr char source[] = R"(
cbuffer DrawConstants : register(b0) { float4 offset; float4 color; };
Texture2DArray<float> shadowDepth : register(t3);
float4 VSMain(float2 position : POSITION) : SV_POSITION {
    return float4(position + offset.xy, 0.0, 1.0);
}
float4 PSMain() : SV_TARGET { return color; }
float4 VSFullscreen(uint index : SV_VertexID) : SV_POSITION {
    float2 position = float2((index << 1) & 2, index & 2);
    return float4(position * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PSShadow() : SV_TARGET {
    float value = shadowDepth.Load(int4(4, 4, 0, 0));
    return float4(value, 0, 1 - value, 1);
}
)";
    ComPtr<ID3DBlob> code, diagnostic;
    const HRESULT result = D3DCompile(source, sizeof(source) - 1, "D3D12RhiTests.hlsl",
        nullptr, nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS |
        D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &code, &diagnostic);
    if (FAILED(result)) {
        const auto message = diagnostic
            ? std::string(static_cast<const char*>(diagnostic->GetBufferPointer()), diagnostic->GetBufferSize())
            : std::string("No shader compiler diagnostic.");
        throw std::runtime_error("D3D12 RHI test shader compilation failed: " + message);
    }
    return code;
}
std::span<const std::byte> bytecode(ID3DBlob* blob) {
    return {static_cast<const std::byte*>(blob->GetBufferPointer()), blob->GetBufferSize()};
}
using Pixel = std::array<unsigned char, 4>;
void expectPixel(const backend::TextureReadback& image, unsigned x, unsigned y, Pixel expected,
    const char* message) {
    require(image.format == rhi::Format::RGBA8Unorm && image.rowPitch >= image.width * 4 &&
        image.bytes.size() >= static_cast<std::size_t>(image.rowPitch) * image.height,
        "Color readback must provide complete RGBA8 rows.");
    require(x < image.width && y < image.height, "Test pixel is outside the readback.");
    const auto offset = static_cast<std::size_t>(y) * image.rowPitch + x * 4;
    for (unsigned channel = 0; channel < expected.size(); ++channel) {
        const int actual = std::to_integer<unsigned char>(image.bytes[offset + channel]);
        if (std::abs(actual - expected[channel]) > 2)
            throw std::runtime_error(std::string(message) + " (channel " + std::to_string(channel) +
                ", actual " + std::to_string(actual) + ").");
    }
}

void verifyUiRetirement(backend::D3D12RhiDevice& device) {
    auto borrowed = device.createTexture({4, 4, 1, rhi::Format::RGBA8Unorm, true, "UI borrowing lifetime"});
    const std::weak_ptr<rhi::Texture> lifetime = borrowed;
    const auto id = device.uiTextureId(borrowed);
    require(id != 0 && device.uiTextureId(borrowed) == id, "A live texture must retain a stable UI descriptor.");
    borrowed.reset();
    device.submitAndWait();
    require(!lifetime.expired(), "An intermediate main-queue flush must not retire a UI-borrowed texture.");
    // No HWND/present is needed: this is the minimized/main-skipped UI boundary.
    device.finishUiFrame();
    require(lifetime.expired(), "Completing all UI work must retire a texture without requiring present.");

    const auto first = device.allocateUiDescriptor();
    device.freeUiDescriptor(first.first);
    rejects([&] { device.freeUiDescriptor(first.first); }, "Double freeing a UI descriptor must fail.");
    const auto second = device.allocateUiDescriptor();
    require(second.first.ptr != first.first.ptr, "A freed UI descriptor cannot be overwritten before the UI completion boundary.");
    device.submitAndWait();
    const auto third = device.allocateUiDescriptor();
    require(third.first.ptr != first.first.ptr, "A main-queue-only flush cannot recycle descriptors still borrowed by platform UI.");
    device.finishUiFrame();
    device.freeUiDescriptor(second.first);
    device.freeUiDescriptor(third.first);
    device.finishUiFrame();
}

void verifyIndependentConstants(backend::D3D12RhiDevice& device, backend::D3D12RhiDevice& foreign) {
    auto& commands = device.commands();
    const auto vsCode = compile("VSMain", "vs_5_0");
    const auto psCode = compile("PSMain", "ps_5_0");
    const auto vs = device.createShader(rhi::ShaderStage::Vertex, bytecode(vsCode.Get()), "Test VS");
    const auto ps = device.createShader(rhi::ShaderStage::Pixel, bytecode(psCode.Get()), "Test PS");
    const std::array<std::array<float, 2>, 3> positions{{
        {-0.35f, -0.65f}, {0.0f, 0.65f}, {0.35f, -0.65f}}};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    const auto vertex = device.createBuffer({sizeof(positions), rhi::BufferUsage::Vertex, true, "Test positions"}, bytes(positions));
    const auto index = device.createBuffer({sizeof(indices), rhi::BufferUsage::Index, true, "Test indices"}, bytes(indices));
    struct Constants { std::array<float, 4> offset, color; };
    static_assert(sizeof(Constants) == 32);
    const auto constants = device.createBuffer({sizeof(Constants), rhi::BufferUsage::Constant, false, "Versioned constants"});
    const auto target = device.createTexture({64, 32, 1, rhi::Format::RGBA8Unorm, true, "Independent draw versions"});
    rhi::GraphicsPipelineDesc description;
    description.vertexShader = vs; description.pixelShader = ps;
    description.attributes = {{"POSITION", 0, 0, 0, 2}};
    description.depthEnable = description.depthWrite = false;
    description.colorFormats = {rhi::Format::RGBA8Unorm};
    description.name = "Independent constants pipeline";
    const auto pipeline = device.createGraphicsPipeline(description);

    rejects([&] { commands.draw(3); }, "A draw outside a pass must fail.");
    rejects([&] { commands.drawIndexed(3); }, "An indexed draw outside a pass must fail.");
    rejects([&] { commands.writeBuffer(vertex, bytes(positions)); }, "Immutable geometry must reject writes.");
    const std::array<std::byte, sizeof(Constants) - 1> partial{};
    rejects([&] { commands.writeBuffer(constants, bytes(partial)); }, "Partial constant writes must fail.");
    rejects([&] { commands.bindIndexBuffer(vertex, 0); }, "A vertex buffer cannot bind as an index buffer.");
    const std::array wrongVertex{rhi::VertexBufferBinding{constants, 8, 0}};
    rejects([&] { commands.bindVertexBuffers(0, wrongVertex); }, "A constant buffer cannot bind as a vertex stream.");
    const std::array wrongConstant{vertex};
    rejects([&] { commands.bindConstantBuffers(rhi::ShaderStage::Vertex, 0, wrongConstant); }, "A vertex buffer cannot bind as a constant buffer.");
    const std::array<rhi::BufferHandle, 1> nullConstants{};
    rejects([&] { commands.bindConstantBuffers(rhi::ShaderStage::Vertex, 9, nullConstants); }, "A null buffer still needs an in-range root slot.");
    const std::array invalidNullVertex{rhi::VertexBufferBinding{nullptr, 8, 0}};
    rejects([&] { commands.bindVertexBuffers(0, invalidNullVertex); }, "An unbound vertex stream must reject a nonzero stride.");
    rejects([&] { commands.bindIndexBuffer(nullptr, 4); }, "An unbound index stream must reject a nonzero offset.");
    rejects([&] { commands.setViewport({-32768, 0, 65535, 32}); }, "Viewport width itself must respect D3D12 limits.");

    const auto foreignConstants = foreign.createBuffer({sizeof(Constants), rhi::BufferUsage::Constant, false, "Foreign constants"});
    const auto foreignTarget = foreign.createTexture({64, 32, 1, rhi::Format::RGBA8Unorm, true, "Foreign target"});
    rejects([&] { commands.writeBuffer(foreignConstants, bytes(Constants{})); }, "Cross-device buffer writes must fail.");
    const std::array foreignBindings{foreignConstants};
    rejects([&] { commands.bindConstantBuffers(rhi::ShaderStage::Pixel, 0, foreignBindings); }, "Cross-device constant bindings must fail.");
    const auto foreignVs = foreign.createShader(rhi::ShaderStage::Vertex, bytecode(vsCode.Get()), "Foreign VS");
    auto invalidDescription = description; invalidDescription.vertexShader = foreignVs;
    rejects([&] { (void)device.createGraphicsPipeline(invalidDescription); }, "A pipeline cannot contain a foreign-device shader.");
    rejects([&] { (void)device.uiTextureId(foreignTarget); }, "A foreign texture cannot supply a UI descriptor.");

    rhi::RenderPassDesc pass;
    pass.name = "Two versions of the same logical constant buffer";
    pass.colors = {{target, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 1, 1}}};
    auto invalidPass = pass; invalidPass.reads = {{target, 0}};
    rejects([&] { commands.beginRenderPass(invalidPass); }, "Pass attachment/sample aliasing must fail.");
    invalidPass = pass; invalidPass.colors[0].texture = foreignTarget;
    rejects([&] { commands.beginRenderPass(invalidPass); }, "A foreign attachment must fail before opening a pass.");
    invalidPass = pass; invalidPass.reads = {{foreignTarget, 0}};
    rejects([&] { commands.beginRenderPass(invalidPass); }, "A foreign sampled texture must fail before opening a pass.");

    commands.beginRenderPass(pass);
    rejects([&] { commands.beginRenderPass(pass); }, "A nested pass must be rejected without ending its owner.");
    rejects([&] { commands.draw(3); }, "A new pass must reject draws without a pipeline.");
    auto mismatchedDescription = description; mismatchedDescription.colorFormats = {rhi::Format::RGBA16Float};
    const auto mismatched = device.createGraphicsPipeline(mismatchedDescription);
    rejects([&] { commands.bindGraphicsPipeline(mismatched); }, "A pipeline must match the active pass formats.");
    commands.bindGraphicsPipeline(pipeline);
    commands.setViewport({0, 0, 64, 32});
    const std::array<rhi::VertexBufferBinding, 1> nullVertex{};
    commands.bindVertexBuffers(0, nullVertex);
    rejects([&] { commands.draw(3); }, "Explicitly unbound vertex attributes must reject a draw.");
    const std::array broadcast{rhi::VertexBufferBinding{vertex, 0, 0}};
    commands.bindVertexBuffers(0, broadcast);
    const std::array vertexBinding{rhi::VertexBufferBinding{vertex, 2 * sizeof(float), 0}};
    commands.bindVertexBuffers(0, vertexBinding);
    commands.bindIndexBuffer(nullptr, 0);
    rejects([&] { commands.drawIndexed(3); }, "An explicitly unbound index stream must reject indexed draws.");
    commands.bindIndexBuffer(index, 0);
    commands.bindConstantBuffers(rhi::ShaderStage::Vertex, 0, nullConstants);
    commands.bindConstantBuffers(rhi::ShaderStage::Pixel, 0, nullConstants);
    const std::array constantBindings{constants};
    commands.bindConstantBuffers(rhi::ShaderStage::Vertex, 0, constantBindings);
    commands.bindConstantBuffers(rhi::ShaderStage::Pixel, 0, constantBindings);
    const Constants left{{-0.5f, 0, 0, 0}, {1, 0, 0, 1}};
    commands.writeBuffer(constants, bytes(left));
    commands.drawIndexed(3, 0, 0);
    const Constants right{{0.5f, 0, 0, 0}, {0, 1, 0, 1}};
    commands.writeBuffer(constants, bytes(right));
    commands.draw(3, 0);
    commands.endRenderPass();
    commands.endRenderPass(); // Must be an idempotent no-op.

    auto image = device.readTexture(target);
    require(image.width == 64 && image.height == 32, "Readback dimensions must match the rendered target.");
    expectPixel(image, 16, 16, {255, 0, 0, 255}, "The earlier draw must retain its red constant version");
    expectPixel(image, 48, 16, {0, 255, 0, 255}, "The later draw must use its independent green constant version");
    expectPixel(image, 32, 1, {0, 0, 255, 255}, "The pass must clear untouched pixels blue");

    pass.colors[0].load = rhi::LoadOp::Load;
    pass.colors[0].clear = {1, 1, 0, 1};
    commands.beginRenderPass(pass);
    rejects([&] { commands.draw(3); }, "Ending the previous pass must retire its pipeline binding.");
    commands.bindGraphicsPipeline(pipeline);
    rejects([&] { commands.draw(3); }, "The following pass must rebind its vertex attributes.");
    commands.bindVertexBuffers(0, vertexBinding);
    rejects([&] { commands.drawIndexed(3); }, "The following pass must rebind its index stream.");
    commands.endRenderPass();
    image = device.readTexture(target);
    expectPixel(image, 16, 16, {255, 0, 0, 255}, "Load must preserve the prior red draw");
    expectPixel(image, 48, 16, {0, 255, 0, 255}, "Load must preserve the prior green draw");
    expectPixel(image, 32, 1, {0, 0, 255, 255}, "Load must not apply the deliberately different clear color");
}

void verifySingleLayerShadow(backend::D3D12RhiDevice& device) {
    auto& commands = device.commands();
    const auto depth = device.createTexture({16, 16, 1, rhi::Format::Depth32Float, true, "Single-layer shadow"});
    const auto color = device.createTexture({16, 16, 1, rhi::Format::RGBA8Unorm, true, "Shadow sample output"});
    rhi::RenderPassDesc clear;
    clear.name = "Clear single shadow layer to one quarter";
    clear.depth = rhi::DepthAttachment{depth, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, 0.25f};
    auto invalid = clear; invalid.depth->layer = 1;
    rejects([&] { commands.beginRenderPass(invalid); }, "A one-layer depth texture must reject layer one.");
    invalid = clear; invalid.depth->clearDepth = std::numeric_limits<float>::quiet_NaN();
    rejects([&] { commands.beginRenderPass(invalid); }, "Depth clear must reject a non-finite value.");
    commands.beginRenderPass(clear);
    commands.endRenderPass();
    const auto rawDepth = device.readTexture(depth);
    require(rawDepth.format == rhi::Format::Depth32Float && rawDepth.width == 16 && rawDepth.height == 16 &&
        rawDepth.rowPitch >= 16 * sizeof(float) && rawDepth.bytes.size() >= rawDepth.rowPitch * rawDepth.height,
        "Depth readback must contain complete raw float rows.");
    for (unsigned y = 0; y < 16; ++y) for (unsigned x = 0; x < 16; ++x) {
        float value = 0;
        std::memcpy(&value, rawDepth.bytes.data() + static_cast<std::size_t>(y) * rawDepth.rowPitch + x * sizeof(float), sizeof(value));
        require(std::isfinite(value) && std::abs(value - 0.25f) < 1e-6f, "Depth clear must initialize every pixel to 0.25.");
    }

    const auto vsCode = compile("VSFullscreen", "vs_5_0");
    const auto psCode = compile("PSShadow", "ps_5_0");
    rhi::GraphicsPipelineDesc description;
    description.vertexShader = device.createShader(rhi::ShaderStage::Vertex, bytecode(vsCode.Get()), "Shadow fullscreen VS");
    description.pixelShader = device.createShader(rhi::ShaderStage::Pixel, bytecode(psCode.Get()), "Shadow array sample PS");
    description.depthEnable = description.depthWrite = false;
    description.colorFormats = {rhi::Format::RGBA8Unorm};
    description.name = "Sample a single-layer Texture2DArray at t3";
    const auto pipeline = device.createGraphicsPipeline(description);
    rhi::RenderPassDesc sample;
    sample.name = "Sample the actual cleared single-layer array";
    sample.colors = {{color, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 0, 1}}};
    sample.reads = {{depth, 3}};
    commands.beginRenderPass(sample);
    commands.setViewport({0, 0, 16, 16});
    commands.bindGraphicsPipeline(pipeline);
    commands.draw(3, 0);
    commands.endRenderPass();
    const auto result = device.readTexture(color);
    expectPixel(result, 0, 0, {64, 0, 191, 255}, "The shader must read 0.25 through a single-layer array SRV");
    expectPixel(result, 8, 8, {64, 0, 191, 255}, "Shadow sampling must cover the actual output center");
    expectPixel(result, 15, 15, {64, 0, 191, 255}, "Shadow sampling must cover the output extent");
}

void expectDepth(const backend::TextureReadback& image, float expected, const char* message) {
    require(image.format == rhi::Format::Depth32Float && image.rowPitch >= image.width * sizeof(float) &&
        image.bytes.size() >= static_cast<std::size_t>(image.rowPitch) * image.height,
        "Depth layer readback must contain complete float rows.");
    for (unsigned y = 0; y < image.height; ++y) for (unsigned x = 0; x < image.width; ++x) {
        float value = 0;
        std::memcpy(&value, image.bytes.data() + static_cast<std::size_t>(y) * image.rowPitch +
            static_cast<std::size_t>(x) * sizeof(float), sizeof(value));
        require(std::isfinite(value) && std::abs(value - expected) < 1e-6f, message);
    }
}

void verifyLayeredCapture(backend::D3D12RhiDevice& device) {
    auto& commands = device.commands();
    const auto source = device.createTexture({16, 16, 2, rhi::Format::Depth32Float, true, "Distinct shadow cascades"});
    const auto snapshot = device.createTexture({16, 16, 1, rhi::Format::Depth32Float, true, "Frozen second cascade"});
    rhi::RenderPassDesc clear;
    clear.name = "Initialize independent shadow array layers";
    for (std::uint32_t layer = 0; layer < 2; ++layer) {
        clear.depth = rhi::DepthAttachment{source, layer, rhi::LoadOp::Clear, rhi::StoreOp::Store,
            layer == 0 ? 0.25f : 0.75f};
        commands.beginRenderPass(clear); commands.endRenderPass();
    }
    rejects([&] { (void)device.readTexture(source, 2); }, "Readback must reject an out-of-range source layer.");
    rejects([&] { device.copyTextureRegion(snapshot, source, 0, 0, 16, 16, 2); },
        "Snapshot copy must reject an out-of-range source layer.");
    device.copyTextureRegion(snapshot, source, 0, 0, 16, 16, 1);
    expectDepth(device.readTexture(source, 0), 0.25f, "Reading layer zero must not select another cascade.");
    expectDepth(device.readTexture(source, 1), 0.75f, "Reading layer one must select the second cascade.");
    expectDepth(device.readTexture(snapshot), 0.75f, "Copied snapshot must contain the requested second cascade.");
    clear.depth->clearDepth = 0.5f;
    commands.beginRenderPass(clear); commands.endRenderPass();
    expectDepth(device.readTexture(source, 1), 0.5f, "Live cascade must accept later changes.");
    expectDepth(device.readTexture(snapshot), 0.75f, "Frozen cascade must survive later writes to its source.");
}

void verifyGpuTimings(backend::D3D12RhiDevice& device) {
    auto& commands = device.commands();
    const auto target = device.createTexture({64, 64, 1, rhi::Format::RGBA8Unorm, true, "Timed target"});
    rhi::RenderPassDesc clear;
    clear.name = "Timed actual GPU clear";
    clear.colors = {{target, rhi::LoadOp::Clear, rhi::StoreOp::Store, {1, 0, 0, 1}}};
    require(!device.pollDiagnosticTiming(), "Timing disabled must not fabricate samples.");
    rejects([&] { device.beginDiagnosticFrame(0); }, "GPU timing must reject an absent frame identity.");
    device.beginDiagnosticFrame(401);
    rejects([&] { device.beginDiagnosticFrame(402); }, "Nested diagnostic frames must preserve their owner.");
    device.beginDiagnosticPass("GPU clear before flush");
    commands.beginRenderPass(clear); commands.endRenderPass();
    device.endDiagnosticPass();
    device.submitAndWait();
    require(!device.pollDiagnosticTiming(), "An intermediate flush cannot publish an incomplete GPU frame.");
    device.beginDiagnosticPass("GPU clear after flush");
    clear.colors[0].clear = {0, 1, 0, 1};
    commands.beginRenderPass(clear); commands.endRenderPass();
    device.endDiagnosticFrame(); // Also closes the open logical diagnostic pass.
    device.endDiagnosticFrame(); device.endDiagnosticPass(); // Idempotent completion.
    require(!device.pollDiagnosticTiming(), "Unsubmitted query resolves must remain pending without a polling wait.");
    device.submitAndWait();
    const auto timing = device.pollDiagnosticTiming();
    require(timing && timing->frameId == 401 && timing->passes.size() == 2,
        "GPU timing must retain the original frame and both passes across submissions.");
    require(std::isfinite(timing->totalMs) && timing->totalMs >= 0,
        "Actual GPU frame timestamps must produce a finite non-negative interval.");
    require(timing->passes[0].first == "GPU clear before flush" && timing->passes[1].first == "GPU clear after flush",
        "Resolved pass names must identify the captured logical scopes.");
    double passTotal = 0;
    for (const auto& pass : timing->passes) {
        require(std::isfinite(pass.second) && pass.second >= 0 && pass.second <= timing->totalMs + 1e-6,
            "Actual GPU pass intervals must be finite and bounded by their own frame.");
        passTotal += pass.second;
    }
    require(passTotal <= timing->totalMs + 1e-6, "Sequential GPU passes cannot exceed their containing frame interval.");
    require(!device.pollDiagnosticTiming(), "A consumed timing sample cannot be reported again.");
    expectPixel(device.readTexture(target), 32, 32, {0, 255, 0, 255}, "Timing instrumentation must preserve actual GPU output");

    // Four ended samples may share one submission; the fifth must be skipped
    // without reusing unresolved slots or inventing a result for that frame.
    for (std::uint64_t frame = 500; frame < 505; ++frame) {
        device.beginDiagnosticFrame(frame);
        device.beginDiagnosticPass("Bounded timing ring");
        device.endDiagnosticFrame();
    }
    require(!device.pollDiagnosticTiming(), "A full unsubmitted timing ring must remain nonblocking and pending.");
    device.submitAndWait();
    for (std::uint64_t frame = 500; frame < 504; ++frame) {
        const auto sample = device.pollDiagnosticTiming();
        require(sample && sample->frameId == frame && sample->passes.size() == 1,
            "A bounded timing ring must preserve every retained frame identity in order.");
    }
    require(!device.pollDiagnosticTiming(), "A skipped fifth timing frame cannot fabricate a sample.");

    device.beginDiagnosticFrame(600);
    for (unsigned pass = 0; pass < 66; ++pass) device.beginDiagnosticPass("Bounded pass count");
    device.endDiagnosticFrame(); device.submitAndWait();
    const auto capped = device.pollDiagnosticTiming();
    require(capped && capped->frameId == 600 && capped->passes.size() == 64,
        "Diagnostic timestamp storage must retain only initialized pass pairs within its fixed budget.");
}

void verifyDebugMessages(backend::D3D12RhiDevice& device) {
    ComPtr<ID3D12InfoQueue> queue;
    if (FAILED(device.device()->QueryInterface(IID_PPV_ARGS(&queue)))) {
        require(!device.validationEnabled() && device.debugMessages().empty(),
            "Missing debug layer must not fabricate native validation messages.");
        return;
    }
    // Runtime defaults or a machine's debug-layer settings may filter INFO.
    // This isolated fixture explicitly enables its own messages and restores
    // the original filter stacks/count limit after validating the real tail.
    struct Filters {
        ID3D12InfoQueue* queue;
        UINT64 previousLimit;
        bool storage = false, retrieval = false;
        ~Filters() {
            if (retrieval) queue->PopRetrievalFilter();
            if (storage) queue->PopStorageFilter();
            (void)queue->SetMessageCountLimit(previousLimit);
        }
    } filters{queue.Get(), queue->GetMessageCountLimit()};
    require(SUCCEEDED(queue->PushEmptyStorageFilter()), "The debug message fixture must control its storage filter.");
    filters.storage = true;
    require(SUCCEEDED(queue->PushEmptyRetrievalFilter()), "The debug message fixture must control its retrieval filter.");
    filters.retrieval = true;
    const auto before = queue->GetNumStoredMessages();
    require(SUCCEEDED(queue->SetMessageCountLimit(std::max(filters.previousLimit, before + 256))),
        "The debug message fixture must reserve storage for every injected message.");
    for (unsigned index = 0; index < 140; ++index) {
        const auto message = "RHI diagnostics test message " + std::to_string(index);
        require(SUCCEEDED(queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, message.c_str())),
            "The active debug layer must accept explicit diagnostic application messages.");
    }
    std::cerr << "[D3D12 messages] stored before=" << before << ", after=" << queue->GetNumStoredMessages()
              << ", retrievable=" << queue->GetNumStoredMessagesAllowedByRetrievalFilter()
              << ", denied=" << queue->GetNumMessagesDeniedByStorageFilter() << '\n';
    require(queue->GetNumStoredMessages() == before + 140,
        "The controlled InfoQueue fixture must really store all 140 injected messages.");
    const auto messages = device.debugMessages();
    require(messages.size() == 128 && messages.back().find("RHI diagnostics test message 139") != std::string::npos,
        "Debug message retrieval must retain the most recent 128 messages.");
    require(messages.back().find("[info]") != std::string::npos && messages.back().find("ID=") != std::string::npos,
        "Debug messages must retain their real severity and native message ID.");
}

void run() {
    // No HWND, D3D11 device or On12 interface participates in this executable.
    // Failure to create native D3D12 is a test failure, never a passing skip.
    const auto device = backend::createD3D12RhiDevice(nullptr, 0, 0, true);
    const auto foreign = backend::createD3D12RhiDevice(nullptr, 0, 0, true);
    require(device && foreign && device->device() && foreign->device(), "Native D3D12 devices must be created.");
    verifyIndependentConstants(*device, *foreign);
    verifySingleLayerShadow(*device);
    verifyUiRetirement(*device);
    verifyLayeredCapture(*device);
    verifyGpuTimings(*device);
    verifyDebugMessages(*device);
    device->waitIdle(); foreign->waitIdle();
    const auto& stats = device->statistics();
    require(stats.drawCalls == 3 && stats.indexedDrawCalls == 1 && stats.bufferUploads >= 4 && stats.renderPasses >= 4,
        "Statistics must describe the actual validated draws, writes and passes.");
    require(device->validationErrorCount() == 0 && foreign->validationErrorCount() == 0,
        "The standalone native D3D12 test must produce zero debug-layer errors.");
    std::cout << "Native D3D12 RHI GPU tests passed on " << device->adapterName()
              << ": independent constant versions, layered shadow snapshots, GPU timestamps, debug messages, validation, UI retirement.\n";
    if (!device->validationEnabled())
        std::cout << "D3D12 debug layer was unavailable; GPU pixel checks still executed.\n";
}
} // namespace

int main() {
    try { run(); return 0; }
    catch (const std::exception& error) {
        std::cerr << "Native D3D12 RHI tests failed: " << error.what() << '\n';
        return 1;
    }
}
