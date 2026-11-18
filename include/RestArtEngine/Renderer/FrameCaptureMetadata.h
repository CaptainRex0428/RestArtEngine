#pragma once

#include <RestArtEngine/Renderer/RenderDiagnostics.h>

#include <string>
#include <string_view>

namespace rae::renderer {

// Version-2 metadata for a .raframe bundle. Actual image bytes are stored by
// the bundle container, not in this JSON. Throws on invalid or oversized input.
[[nodiscard]] std::string encodeCaptureMetadata(const RenderFrameCapture& capture);

// Validates the complete schema, sizes and values before replacing capture.
// Failure returns an explanation and leaves the previous capture untouched.
bool decodeCaptureMetadata(std::string_view json, RenderFrameCapture& capture, std::string& error);

} // namespace rae::renderer
