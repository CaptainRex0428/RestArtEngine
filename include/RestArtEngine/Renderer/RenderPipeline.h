#pragma once

// Declarative description of how one scene frame is organized into passes.
//
// Three independent dimensions are deliberately kept apart:
//   - render path (this header): Forward / Deferred, i.e. the *pipeline shape*;
//   - render system (future): PBR / NPR, i.e. the shading model family;
//   - graphics API (RenderBackend.h): DX11 / DX12 / Vulkan / OpenGL.
//
// A declaration is data only: pass order plus the GPU resources each pass reads
// and writes. The DX11 executor in SceneRenderer consumes the same list that
// diagnostics and tests inspect, so documentation, profiling and execution
// cannot drift apart. See docs/RenderPipeline.md for the guided tour.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rae::renderer {

// How one frame is split into stages. This is not the shading model family and
// not the graphics API; a future NPR system can reuse either path.
enum class RenderPath { Forward, Deferred };

// Direct-light shadowing strategy. Raster variants share the persistent depth
// array; GTAO and ContactShadow additionally use the Deferred G-buffer and fall
// back to the raster map under Forward. AnalyticRay traces built-in primitives
// plus the common mesh-triangle scene and runs no raster shadow pass.
enum class ShadowMethod {
    ShadowMap,
    AnalyticRay,
    Disabled,
    ShadowMapHard,
    ShadowMapPCF9,
    ShadowMapPCSS,
    ShadowMapCSM2,
    GTAO,
    ContactShadow,
};

// GPU resources that can form a producer/consumer edge between passes.
// Constant buffers and samplers are implicit bindings owned by the renderer;
// they are not data-flow edges and are intentionally absent here.
enum class PipelineResource : std::uint64_t {
    None = 0,
    SceneColor = 1ull << 0,
    SceneDepth = 1ull << 1,
    ShadowMap = 1ull << 2,
    GBufferAlbedo = 1ull << 3,
    GBufferNormal = 1ull << 4,
    GBufferPosition = 1ull << 5,
    RayTriangleScene = 1ull << 6,
    IndirectLighting = 1ull << 7,
    HardwareImage = 1ull << 8,
    SelectionMask = 1ull << 9,
    DepthOfFieldSource = 1ull << 10,
    FrameCapture = 1ull << 11,
};

constexpr PipelineResource operator|(PipelineResource left, PipelineResource right) noexcept {
    return static_cast<PipelineResource>(
        static_cast<std::uint64_t>(left) | static_cast<std::uint64_t>(right));
}
constexpr PipelineResource operator&(PipelineResource left, PipelineResource right) noexcept {
    return static_cast<PipelineResource>(
        static_cast<std::uint64_t>(left) & static_cast<std::uint64_t>(right));
}
constexpr PipelineResource& operator|=(PipelineResource& left, PipelineResource right) noexcept {
    left = left | right;
    return left;
}
// True when every bit of `value` is set in `set`. PipelineResource::None is
// vacuously contained and should be handled by callers before formatting.
constexpr bool contains(PipelineResource set, PipelineResource value) noexcept {
    return (static_cast<std::uint64_t>(set) & static_cast<std::uint64_t>(value)) ==
        static_cast<std::uint64_t>(value);
}

// One rendering stage. `name` is the stable profiler/debug label and must stay
// unique within a frame so diagnostics and capture events can be attributed.
enum class RenderPassKind : std::uint8_t {
    ShadowMap,
    HardwareTransport,
    GBuffer,
    DeferredLighting,
    ForwardBackground,
    ForwardGeometry,
    HardwareResolve,
    DepthOfField,
    SelectionMask,
    SelectionComposite,
    FrameCapture,
};

struct RenderPassDeclaration {
    RenderPassKind kind = RenderPassKind::GBuffer;
    std::string_view name{};
    PipelineResource reads = PipelineResource::None;
    PipelineResource writes = PipelineResource::None;
};

struct RenderPipeline {
    RenderPath path = RenderPath::Deferred;
    std::vector<RenderPassDeclaration> passes;
};

// Per-frame decisions that determine which passes exist. Capabilities are
// already resolved by the caller: `hardwareTransport` is false unless the DXR
// frame is actually dispatched this frame, and `depthOfField` is false unless
// the screen-space pass is eligible. The declaration therefore describes what
// executes, not what the user wishes were possible; the wish/effect split lives
// in RenderFeatures.h.
struct PipelineBuildInputs {
    RenderPath path = RenderPath::Deferred;
    bool rasterShadowMap = false;
    bool rayTriangleScene = false;
    bool hardwareTransport = false;
    bool realtimeIndirect = false;
    bool depthOfField = false;
    bool selectionOutline = false;
    bool frameCapture = false;
};

// Resources supplied outside the pass list: the renderer prepares the
// world-space triangle scene before the frame, so validation accepts it as an
// input instead of demanding a producer pass.
inline constexpr PipelineResource pipelineFrameInputs = PipelineResource::RayTriangleScene;

[[nodiscard]] RenderPipeline buildScenePipeline(const PipelineBuildInputs& inputs);

struct PipelineValidation {
    bool valid = true;
    std::string error;
};

// Checks the declaration's internal consistency: every read must be produced
// earlier (or be a frame input), the path must contain its required passes and
// must not contain the other path's passes, and something must produce
// SceneColor. A failure is a programming error and aborts the frame.
[[nodiscard]] PipelineValidation validatePipeline(const RenderPipeline& pipeline);

// Human-readable pass order and resource edges, one pass per line. Used by the
// diagnostics panel, logs and tests.
[[nodiscard]] std::string describePipeline(const RenderPipeline& pipeline);
[[nodiscard]] std::string describePipelineResources(PipelineResource resources);
[[nodiscard]] const char* toString(RenderPath path) noexcept;
[[nodiscard]] const char* toString(RenderPassKind kind) noexcept;

} // namespace rae::renderer
