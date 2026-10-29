#pragma once

namespace rae::assets {

// Shared by the picker, external drops and format readers. Persisted editor
// preferences choose the default; a caller can request strict validation.
struct AssetImportOptions {
    bool skipDegenerateTriangles = true;
};

} // namespace rae::assets
