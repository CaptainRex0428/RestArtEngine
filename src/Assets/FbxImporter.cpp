#include "FbxImporter.h"
#include "RestArtEngine/Assets/AssetLibrary.h"
#include <ufbx.h>

#include <array>
#include <cmath>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace rae::assets::detail {
namespace {
constexpr std::size_t MaxFileBytes = 256 * 1024 * 1024;
constexpr std::size_t MaxParserMemory = 512 * 1024 * 1024;
constexpr std::size_t MaxMeshCorners = 12000000;
constexpr std::size_t MaxPolygonCorners = 65536;
constexpr double MaxCoordinate = 1e9;

std::string utf8Path(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

void validateVector(ufbx_vec3 value, const char* description) {
    for (double component : value.v) {
        if (!std::isfinite(component) || std::abs(component) > MaxCoordinate)
            throw std::runtime_error(std::string("FBX ") + description +
                " exceeds the finite supported range (+/-1e9).");
    }
}

bool denyExternalFile(void*, ufbx_stream*, const char*, std::size_t, const ufbx_open_file_info*) {
    return false;
}

void validateMesh(const ufbx_mesh& mesh) {
    // Exporters can retain empty/point/line mesh nodes alongside triangles.
    // These do not require renderable normal attributes and add no geometry.
    if (!mesh.num_triangles) return;
    if (!mesh.vertex_position.exists)
        throw std::runtime_error("FBX mesh is missing positions.");
    for (const auto& position : mesh.vertex_position.values) validateVector(position, "vertex");
    // Finite zero normals can be repaired for retained triangles. Keep invalid
    // numeric attributes a hard error, including in unreferenced entries.
    for (const auto& normal : mesh.vertex_normal.values) validateVector(normal, "normal");
    if (mesh.num_triangles > MaxMeshCorners / 3)
        throw std::runtime_error("FBX exceeds the editor mesh size limit (12 million triangle corners).");
    for (std::size_t faceIndex = 0; faceIndex < mesh.faces.count; ++faceIndex) {
        const auto face = mesh.faces.data[faceIndex];
        // Point/line primitives are valid in FBX but cannot become static mesh triangles.
        if (face.num_indices < 3) continue;
        if (face.num_indices > MaxPolygonCorners)
            throw std::runtime_error("FBX polygon exceeds the 65536-corner limit.");
        if (faceIndex < mesh.face_hole.count && mesh.face_hole.data[faceIndex])
            throw std::runtime_error("FBX polygon holes are not supported by static mesh import.");
        if (face.index_begin > mesh.num_indices || face.num_indices > mesh.num_indices - face.index_begin)
            throw std::runtime_error("FBX polygon indices are out of range.");
    }
}

void appendInstance(MeshAsset& result, const ufbx_node& node, MeshImportDiagnostics& diagnostics,
    const AssetImportOptions& options) {
    const auto& mesh = *node.mesh;
    if (!mesh.num_triangles) return;
    const auto& transform = node.geometry_to_world;
    for (double component : transform.v) {
        if (!std::isfinite(component)) throw std::runtime_error("FBX node transform is not finite.");
    }
    const auto determinant = ufbx_matrix_determinant(&transform);
    if (!std::isfinite(determinant) || determinant == 0.0)
        throw std::runtime_error("FBX mesh instance has a singular or invalid transform.");

    // With TRANSFORM_ROOT + MIRROR_AXIS_NONE the axes conversion remains in
    // geometry_to_world. ufbx has not reversed the source polygon winding, so
    // this one determinant covers both axis conversion and mirrored instances.
    const bool reverseWinding = determinant < 0.0;
    // ufbx returns inverse-transpose times abs(det); normalizing removes that
    // scale while preserving the correct sign for mirrored transforms.
    const auto normalTransform = ufbx_matrix_for_normals(&transform);
    for (double component : normalTransform.v) {
        if (!std::isfinite(component)) throw std::runtime_error("FBX normal transform is not finite.");
    }
    std::vector<std::uint32_t> triangleIndices(mesh.max_face_triangles * 3);
    for (const auto face : mesh.faces) {
        if (face.num_indices < 3) continue;
        ufbx_panic panic{};
        const auto triangleCount = ufbx_catch_triangulate_face(&panic, triangleIndices.data(),
            triangleIndices.size(), &mesh, face);
        if (panic.did_panic || triangleCount != face.num_indices - 2)
            throw std::runtime_error("FBX polygon could not be triangulated.");
        const auto cornerCount = std::size_t(triangleCount) * 3;
        if (cornerCount > MaxMeshCorners - result.vertices.size())
            throw std::runtime_error("FBX exceeds the editor mesh size limit (12 million triangle corners).");
        for (std::size_t triangle = 0; triangle < cornerCount; triangle += 3) {
            std::array<MeshVertex, 3> vertices{};
            std::array<ufbx_vec3, 3> sourceNormals{};
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const auto orderedCorner = reverseWinding && corner != 0 ? 3 - corner : corner;
                const auto index = triangleIndices[triangle + orderedCorner];
                if (index >= mesh.num_indices || index >= mesh.vertex_position.indices.count)
                    throw std::runtime_error("FBX triangulated vertex index is out of range.");
                const auto positionIndex = mesh.vertex_position.indices.data[index];
                if (positionIndex >= mesh.vertex_position.values.count)
                    throw std::runtime_error("FBX vertex attribute index is out of range.");
                if (mesh.vertex_normal.exists) {
                    if (index >= mesh.vertex_normal.indices.count)
                        throw std::runtime_error("FBX triangulated normal index is out of range.");
                    const auto normalIndex = mesh.vertex_normal.indices.data[index];
                    if (normalIndex >= mesh.vertex_normal.values.count)
                        throw std::runtime_error("FBX normal attribute index is out of range.");
                    sourceNormals[corner] = mesh.vertex_normal.values.data[normalIndex];
                }
                const auto position = ufbx_transform_position(&transform, mesh.vertex_position.values.data[positionIndex]);
                validateVector(position, "transformed vertex");
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    vertices[corner].position[axis] = static_cast<float>(position.v[axis]);
                }
            }
            // Validate the stored float geometry as well: large translations or
            // very small scales can collapse otherwise valid double triangles.
            const double ux = double(vertices[1].position[0]) - vertices[0].position[0];
            const double uy = double(vertices[1].position[1]) - vertices[0].position[1];
            const double uz = double(vertices[1].position[2]) - vertices[0].position[2];
            const double vx = double(vertices[2].position[0]) - vertices[0].position[0];
            const double vy = double(vertices[2].position[1]) - vertices[0].position[1];
            const double vz = double(vertices[2].position[2]) - vertices[0].position[2];
            const ufbx_vec3 cross{{uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx}};
            const auto area = std::hypot(cross.x, cross.y, cross.z);
            if (!std::isfinite(area))
                throw std::runtime_error("FBX triangle area is not finite after static mesh conversion.");
            if (area == 0.0) {
                if (!options.skipDegenerateTriangles)
                    throw std::runtime_error("FBX contains a degenerate triangle after static mesh conversion. "
                        "Enable Skip degenerate triangles or clean the source mesh.");
                ++diagnostics.skippedDegenerateTriangles;
                continue;
            }
            // Test the stored positions first: discarded geometry must not fail
            // because its generated normals are zero. There is no area epsilon;
            // even very small triangles survive if their float positions do.
            const ufbx_vec3 geometricNormal{{cross.x / area, cross.y / area, cross.z / area}};
            for (std::size_t corner = 0; corner < 3; ++corner) {
                auto normal = ufbx_transform_direction(&normalTransform, sourceNormals[corner]);
                const auto length = std::hypot(normal.x, normal.y, normal.z);
                if (!std::isfinite(length))
                    throw std::runtime_error("FBX contains a non-finite transformed normal.");
                if (length == 0.0) {
                    normal = geometricNormal;
                    ++diagnostics.repairedNormals;
                } else {
                    for (double& component : normal.v) component /= length;
                }
                for (std::size_t axis = 0; axis < 3; ++axis)
                    vertices[corner].normal[axis] = static_cast<float>(normal.v[axis]);
            }
            for (const auto& vertex : vertices) {
                result.indices.push_back(static_cast<std::uint32_t>(result.vertices.size()));
                result.vertices.push_back(vertex);
            }
        }
    }
}
} // namespace

bool readFbxMesh(const std::filesystem::path& path, MeshAsset& output, std::string& error,
    MeshImportDiagnostics* diagnostics, const AssetImportOptions& options) {
    if (diagnostics) *diagnostics = {};
    try {
        if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("FBX source is not a regular file.");
        // std::filesystem::path keeps Unicode filenames intact on Windows.
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream) throw std::runtime_error("Cannot open FBX: " + utf8Path(path));
        const auto fileSize = stream.tellg();
        if (fileSize <= 0) throw std::runtime_error("FBX source is empty or unreadable.");
        if (fileSize > std::streamoff(MaxFileBytes)) throw std::runtime_error("FBX source exceeds the 256 MiB file size limit.");
        std::vector<char> bytes(static_cast<std::size_t>(fileSize));
        stream.seekg(0);
        if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) || stream.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("Failed reading FBX or the source changed while reading.");

        ufbx_load_opts loadOptions{};
        loadOptions.file_format = UFBX_FILE_FORMAT_FBX;
        loadOptions.strict = true;
        loadOptions.index_error_handling = UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING;
        loadOptions.generate_missing_normals = true;
        loadOptions.ignore_animation = true;
        loadOptions.ignore_embedded = true;
        loadOptions.skip_skin_vertices = true;
        loadOptions.skip_mesh_parts = true;
        loadOptions.load_external_files = false;
        loadOptions.open_file_cb.fn = denyExternalFile;
        loadOptions.node_depth_limit = 256;
        loadOptions.target_axes = ufbx_axes_left_handed_y_up;
        loadOptions.target_unit_meters = 1.0;
        loadOptions.space_conversion = UFBX_SPACE_CONVERSION_TRANSFORM_ROOT;
        loadOptions.handedness_conversion_axis = UFBX_MIRROR_AXIS_NONE;
        loadOptions.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_PRESERVE;
        loadOptions.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_PRESERVE;
        loadOptions.temp_allocator.memory_limit = MaxParserMemory;
        loadOptions.result_allocator.memory_limit = MaxParserMemory;
        ufbx_error parseError{};
        const std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> scene(
            ufbx_load_memory(bytes.data(), bytes.size(), &loadOptions, &parseError), ufbx_free_scene);
        if (!scene) {
            char description[1024]{};
            ufbx_format_error(description, sizeof(description), &parseError);
            throw std::runtime_error(std::string("Cannot parse FBX: ") + description);
        }
        if (scene->metadata.file_format != UFBX_FILE_FORMAT_FBX)
            throw std::runtime_error("Source content is not FBX.");
        if (!ufbx_coordinate_axes_valid(scene->settings.axes) || !std::isfinite(scene->settings.unit_meters) ||
            scene->settings.unit_meters <= 0.0)
            throw std::runtime_error("FBX has invalid coordinate axes or units.");
        for (const auto* mesh : scene->meshes) validateMesh(*mesh);

        std::size_t totalCorners = 0;
        for (const auto* node : scene->nodes) {
            if (!node->mesh) continue;
            if (node->mesh->num_triangles > (MaxMeshCorners - totalCorners) / 3)
                throw std::runtime_error("FBX instances exceed the editor mesh size limit (12 million triangle corners).");
            totalCorners += node->mesh->num_triangles * 3;
        }
        if (!totalCorners) throw std::runtime_error("FBX contains no renderable mesh faces.");
        MeshAsset result;
        result.name = utf8Path(path.stem());
        result.primitive = MeshPrimitive::Imported;
        result.sourcePath = utf8Path(std::filesystem::absolute(path).lexically_normal());
        result.vertices.reserve(totalCorners);
        result.indices.reserve(totalCorners);
        MeshImportDiagnostics importDiagnostics;
        for (const auto* node : scene->nodes) if (node->mesh) appendInstance(result, *node, importDiagnostics, options);
        if (result.indices.empty()) throw std::runtime_error("FBX contains no renderable mesh faces after skipping " +
            std::to_string(importDiagnostics.skippedDegenerateTriangles) + " degenerate triangle(s).");
        output = std::move(result);
        if (diagnostics) *diagnostics = importDiagnostics;
        error.clear();
        return true;
    } catch (const std::bad_alloc&) {
        error = "FBX import ran out of memory within the configured import limits.";
        return false;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}
} // namespace rae::assets::detail
