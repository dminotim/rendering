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
     * 24 bytes. Geometry is read out of a storage buffer indexed by vertex id rather than through
     * a vertex attribute description, so the layout lives in the shader and every model must
     * arrive in this exact shape.
     *
     * @note Every member is a four-byte scalar on purpose. A `vec3` inside a storage buffer array
     *       has 16-byte alignment, which is why the previous layout needed padding to 48 bytes;
     *       a struct of scalars aligns to 4 and therefore packs tightly. On a ten-million-triangle
     *       scene that difference is a couple of hundred megabytes.
     */
    struct MeshVertex {
        float    position[3];   ///< Full precision: positions are what depth precision rests on.
        /**
         * @brief Normal, octahedron-mapped into two signed 16-bit values.
         *
         * Roughly 0.1 degrees of error — far below what shading can show. Decoded in the shader
         * with `unpackSnorm2x16` (GLSL) or `as_type<short2>` (MSL).
         */
        uint32_t packedNormal;
        /**
         * @brief Texture coordinate, full precision.
         *
         * Deliberately not half-float. Scenes in the archive tile textures with coordinates in
         * the tens, and at that magnitude a half has a step of about 0.03 of a texture — visible
         * swimming on floors and walls.
         */
        float    uv[2];
    };
    static_assert(sizeof(MeshVertex) == 24, "MeshVertex must match the shader's array stride");

    /// @brief Packs a unit normal into two signed 16-bit values via the octahedron mapping.
    uint32_t packNormal(float x, float y, float z);

    /// @brief Recovers a unit normal from packNormal(), for transforming already-packed geometry.
    void unpackNormal(uint32_t packed, float out[3]);

    /**
     * @enum MaterialBlendMode
     * @brief How a material's coverage is resolved, which decides when it is drawn.
     *
     * The distinction matters more than it looks. Cutout is *not* transparency: coverage is
     * binary, so the depth buffer handles it and no sorting is needed. Only Transparent needs
     * back-to-front ordering.
     */
    enum class MaterialBlendMode {
        Opaque,       ///< Fully covers. Drawn first, sorted front-to-back for early-Z.
        Cutout,       ///< Binary coverage via discard. Drawn with the opaque set.
        Transparent   ///< Real blending. Drawn last, back-to-front, without depth writes.
    };

    /**
     * @struct MeshMaterial
     * @brief The subset of a material this renderer understands.
     *
     * Formats carry far more than this. These are the fields that change what appears on screen
     * for the scenes in the archive, which are authored in classic Phong MTL rather than in any
     * physically based workflow — so several values here are derived rather than read.
     */
    struct MeshMaterial {
        std::string name;

        float baseColor[3] = { 0.8f, 0.8f, 0.8f };
        float emissive[3]  = { 0.0f, 0.0f, 0.0f };
        float opacity      = 1.0f;   ///< MTL `d`. Below 1 means real transparency.
        float roughness    = 0.5f;   ///< Derived from `Ns`; see Mesh.cpp for the conversion.
        float metallic     = 0.0f;   ///< MTL has no such concept; heuristically derived from `Ks`.

        std::filesystem::path albedoTexture;    ///< map_Kd. Colour — needs sRGB decoding.
        std::filesystem::path alphaTexture;     ///< map_d. Data — never sRGB.
        std::filesystem::path normalTexture;    ///< map_bump / bump. Data — never sRGB.

        MaterialBlendMode blendMode = MaterialBlendMode::Opaque;

        /**
         * @brief Whether back faces should be kept.
         *
         * The archive is full of foliage and cloth modelled as single-sided planes meant to be
         * seen from both directions. MTL has no flag for it, so this is a heuristic: anything
         * with an alpha mask is treated as two-sided, which covers exactly those cases.
         */
        bool twoSided = false;
    };

    /**
     * @struct MeshSubset
     * @brief A contiguous run of indices sharing one material.
     *
     * Carries its own bounds because culling at whole-model granularity achieves nothing — a
     * courtyard is always partly in view. Per-subset bounds are the coarsest granularity at which
     * frustum culling starts to pay.
     */
    struct MeshSubset {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        int32_t  materialIndex = -1;   ///< Index into Mesh::materials, or -1 for the default.

        float boundsMin[3] = {  1e30f,  1e30f,  1e30f };
        float boundsMax[3] = { -1e30f, -1e30f, -1e30f };

        std::array<float, 3> center() const {
            return { (boundsMin[0] + boundsMax[0]) * 0.5f,
                     (boundsMin[1] + boundsMax[1]) * 0.5f,
                     (boundsMin[2] + boundsMax[2]) * 0.5f };
        }
        /// @brief Radius of a sphere enclosing the bounds.
        float radius() const;
    };

    /**
     * @struct Mesh
     * @brief A loaded scene, already in the form the renderer wants.
     *
     * Loading normalises everything: triangulated, indexed, with normals present whether or not
     * the file had them, with bounds precomputed per subset, and with materials classified by
     * blend mode.
     */
    struct Mesh {
        std::vector<MeshVertex>   vertices;
        std::vector<uint32_t>     indices;
        std::vector<MeshSubset>   subsets;
        std::vector<MeshMaterial> materials;

        float boundsMin[3] = {  1e30f,  1e30f,  1e30f };
        float boundsMax[3] = { -1e30f, -1e30f, -1e30f };

        bool hadNormals   = false;
        bool hadTexCoords = false;
        std::string sourceFormat;

        /// @brief Directory the model was loaded from; texture paths resolve against it.
        std::filesystem::path baseDirectory;

        bool empty() const { return vertices.empty() || indices.empty(); }

        std::array<float, 3> center() const {
            return { (boundsMin[0] + boundsMax[0]) * 0.5f,
                     (boundsMin[1] + boundsMax[1]) * 0.5f,
                     (boundsMin[2] + boundsMax[2]) * 0.5f };
        }

        /// @brief Longest side of the bounding box. Used to scale camera speed and clip planes.
        float boundsExtent() const;

        /// @brief Bytes the geometry occupies, for the memory report.
        size_t geometryBytes() const {
            return vertices.size() * sizeof(MeshVertex) + indices.size() * sizeof(uint32_t);
        }
    };

    /**
     * @brief Loads a model, dispatching on file extension.
     * @param path The .obj, .stl, .ply or .fbx file.
     * @param[out] error Human-readable reason on failure.
     * @return The loaded mesh, or an empty one on failure.
     */
    Mesh loadMesh(const std::filesystem::path& path, std::string& error);

    /**
     * @brief Reads a binary FBX. Defined in FbxLoader.cpp.
     *
     * Separate from the other readers because FBX is a container rather than a geometry format:
     * the parser that gets to the vertices is most of the file, and none of it is shared with the
     * line-oriented readers in Mesh.cpp.
     */
    bool loadFbx(const std::filesystem::path& path, Mesh& mesh, std::string& error);

    /**
     * @brief Reads a `.dmscene` kit layout. Defined in SceneKit.cpp.
     *
     * Asset kits ship every prop at the origin with no placement data, so the arrangement has to
     * come from somewhere: this reads a small hand-editable text file naming assets, positions
     * and yaws, and merges the referenced models into one mesh.
     */
    bool loadScene(const std::filesystem::path& path, Mesh& mesh, std::string& error);

    // ─────────────────────────────────────────────────────────────────────────
    // Binary cache
    //
    // Parsing a gigabyte of text OBJ takes tens of seconds, and half of that is deduplicating
    // vertices. Doing it once and saving the result turns every subsequent start into a handful
    // of large reads.
    // ─────────────────────────────────────────────────────────────────────────

    /// @brief The cache file that belongs to @p modelPath.
    std::filesystem::path sceneCachePath(const std::filesystem::path& modelPath);

    /**
     * @brief Loads a cached scene if one exists and is still valid for @p modelPath.
     *
     * Validity means the magic and version match, and the source file's size and modification
     * time are unchanged. Anything else is treated as a miss rather than an error.
     */
    bool loadSceneCache(const std::filesystem::path& modelPath, Mesh& mesh);

    /// @brief Writes @p mesh to the cache file for @p modelPath. Failure is not fatal.
    bool saveSceneCache(const std::filesystem::path& modelPath, const Mesh& mesh);

} // namespace dmrender

#endif //RENDERING_MESH_HPP
