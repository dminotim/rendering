//
// Created by Artem Avdoshkin on 16.08.2025.
//

#ifndef RENDERING_MESH_HPP
#define RENDERING_MESH_HPP

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dmrender {

    /**
     * @struct MeshVertex
     * @brief One vertex in the canonical layout every loaded model is converted to.
     *
     * The renderer reads geometry out of a storage buffer indexed by vertex id rather than
     * through a vertex attribute description, so the layout lives in the shader and every model
     * must arrive in this exact shape. That is the trade the abstraction makes: no per-asset
     * vertex formats, but also no vertex layout plumbing.
     *
     * @note 48 bytes, and every member is 16-byte aligned on purpose. A `vec3` inside a storage
     *       buffer array has a 16-byte stride in shader memory layout rules, so padding here is
     *       not waste — it is what makes the C++ struct and the shader struct agree byte for
     *       byte. Getting this wrong produces geometry that looks sheared or exploded.
     */
    struct MeshVertex {
        float position[3];
        float pad0 = 0.0f;
        float normal[3];
        float pad1 = 0.0f;
        float uv[2];
        float pad2[2] = { 0.0f, 0.0f };
    };
    static_assert(sizeof(MeshVertex) == 48, "MeshVertex must match the shader's array stride");

    /**
     * @struct MeshMaterial
     * @brief The subset of a material this viewer understands.
     *
     * Formats carry far more than this — specular exponents, emission, illumination models —
     * but a viewer that renders a base colour and one texture needs only these. Fields are
     * populated when the format supplies them and left at their defaults otherwise.
     */
    struct MeshMaterial {
        std::string name;
        float baseColor[3] = { 0.8f, 0.8f, 0.8f };
        /// Path to the albedo texture, resolved relative to the model file. Empty if none.
        std::filesystem::path albedoTexture;
    };

    /**
     * @struct MeshSubset
     * @brief A contiguous run of indices sharing one material.
     *
     * A model with several materials becomes several subsets over one shared vertex and index
     * buffer, so switching material costs a pipeline-state change rather than a buffer rebind.
     */
    struct MeshSubset {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        int32_t materialIndex = -1;   ///< Index into Mesh::materials, or -1 for the default.
    };

    /**
     * @struct Mesh
     * @brief A loaded model, already in the form the renderer wants.
     *
     * Loading normalises everything: triangulated, indexed, with normals present whether or not
     * the file had them, and with bounds precomputed so a camera can frame the model without a
     * second pass over the data.
     */
    struct Mesh {
        std::vector<MeshVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<MeshSubset> subsets;
        std::vector<MeshMaterial> materials;

        /// Axis-aligned bounds of the geometry.
        std::array<float, 3> boundsMin = { 0.0f, 0.0f, 0.0f };
        std::array<float, 3> boundsMax = { 0.0f, 0.0f, 0.0f };

        /// True when the file supplied normals; false when they were generated on load.
        bool hadNormals = false;
        /// True when the file supplied texture coordinates.
        bool hadTexCoords = false;

        std::string sourceFormat;   ///< "OBJ", "STL" or "PLY", for reporting.

        std::array<float, 3> center() const {
            return { (boundsMin[0] + boundsMax[0]) * 0.5f,
                     (boundsMin[1] + boundsMax[1]) * 0.5f,
                     (boundsMin[2] + boundsMax[2]) * 0.5f };
        }

        /// @brief Longest edge of the bounding box; 0 for an empty mesh.
        float boundsExtent() const {
            const float dx = boundsMax[0] - boundsMin[0];
            const float dy = boundsMax[1] - boundsMin[1];
            const float dz = boundsMax[2] - boundsMin[2];
            return dx > dy ? (dx > dz ? dx : dz) : (dy > dz ? dy : dz);
        }

        bool empty() const { return vertices.empty() || indices.empty(); }
    };

    /**
     * @brief Loads a model, choosing the reader by file extension.
     *
     * Supported: `.obj` (with materials and texture references), `.stl` (binary and ASCII),
     * `.ply` (binary and ASCII).
     *
     * Whatever the format, the result is triangulated, indexed and carries normals. Files
     * without normals get smooth ones generated; files without texture coordinates get zeroes,
     * which is harmless because a model with no texture is not sampled anyway.
     *
     * @param path Path to the model file.
     * @param error Receives a human-readable description if loading fails.
     * @return The loaded mesh, or an empty mesh on failure.
     */
    Mesh loadMesh(const std::filesystem::path& path, std::string& error);

} // namespace dmrender

#endif //RENDERING_MESH_HPP
