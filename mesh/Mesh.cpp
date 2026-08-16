#include "Mesh.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>

// tinyobjloader refuses streams above 256 MiB by default — a sane guard against a corrupt file
// being read into memory, and far below what the archive actually ships. San Miguel is 1.1 GB of
// text (600 MB for the low-poly variant). The macro exists precisely to be raised; 4 GiB keeps
// the guard meaningful while covering everything in the archive.
#define TINYOBJLOADER_STREAM_READER_MAX_BYTES (size_t(4) * size_t(1024) * size_t(1024) * size_t(1024))
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include "happly.h"
#include "microstl.h"

namespace dmrender {

    // ─────────────────────────────────────────────────────────────────────────
    // Normal packing
    // ─────────────────────────────────────────────────────────────────────────

    uint32_t packNormal(float x, float y, float z)
    {
        const float length = std::sqrt(x * x + y * y + z * z);
        if (length < 1e-20f) { x = 0.0f; y = 1.0f; z = 0.0f; }
        else                 { x /= length; y /= length; z /= length; }

        // Octahedron mapping: project the unit sphere onto the |x|+|y|+|z| = 1 octahedron, then
        // unfold its lower half outwards into the corners of the square, so the whole sphere
        // covers [-1,1]^2 exactly once.
        const float invL1 = 1.0f / (std::abs(x) + std::abs(y) + std::abs(z));
        float px = x * invL1;
        float py = y * invL1;

        if (z < 0.0f) {
            const float fx = (1.0f - std::abs(py)) * (px >= 0.0f ? 1.0f : -1.0f);
            const float fy = (1.0f - std::abs(px)) * (py >= 0.0f ? 1.0f : -1.0f);
            px = fx; py = fy;
        }

        // Signed 16-bit normalised, matching GLSL's unpackSnorm2x16 exactly.
        auto quantise = [](float value) -> uint32_t {
            const float clamped = std::max(-1.0f, std::min(1.0f, value));
            const int32_t q = static_cast<int32_t>(std::lround(clamped * 32767.0f));
            return static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(q)));
        };
        return quantise(px) | (quantise(py) << 16);
    }

    namespace {

        // ─────────────────────────────────────────────────────────────────────
        // Vertex deduplication
        //
        // An OBJ addresses position, normal and texture coordinate with independent indices,
        // which no GPU can do — it wants one index per vertex. Collapsing each unique
        // combination into a single vertex bridges the two models.
        //
        // A hash map keyed on the triple is the obvious implementation and the wrong one at this
        // scale: San Miguel has around thirty million face-vertices, and a table sized for that
        // costs a gigabyte before storing a single vertex. Instead each *position* owns a short
        // chain of the vertices created from it, which is where the combinations actually cluster
        // — a position typically has one to six distinct normal/uv pairs. Memory is four bytes
        // per position plus sixteen per emitted vertex, and lookups stay O(chain length).
        // ─────────────────────────────────────────────────────────────────────
        class VertexDeduplicator {
        public:
            explicit VertexDeduplicator(size_t positionCount)
                : m_head(positionCount, -1) {}

            void reserve(size_t expectedVertices)
            {
                m_next.reserve(expectedVertices);
                m_normal.reserve(expectedVertices);
                m_texCoord.reserve(expectedVertices);
                m_vertex.reserve(expectedVertices);
            }

            /// @return Index of the vertex for this attribute triple, creating it if new.
            template <typename EmitFn>
            uint32_t resolve(int position, int normal, int texCoord, EmitFn&& emit)
            {
                if (position < 0 || static_cast<size_t>(position) >= m_head.size()) {
                    // Malformed index. Emit an unshared vertex rather than reject the file.
                    return emit();
                }

                for (int32_t entry = m_head[position]; entry >= 0; entry = m_next[entry]) {
                    if (m_normal[entry] == normal && m_texCoord[entry] == texCoord) {
                        return m_vertex[entry];
                    }
                }

                const uint32_t created = emit();
                m_next.push_back(m_head[position]);
                m_normal.push_back(normal);
                m_texCoord.push_back(texCoord);
                m_vertex.push_back(created);
                m_head[position] = static_cast<int32_t>(m_next.size()) - 1;
                return created;
            }

        private:
            std::vector<int32_t>  m_head;      ///< First chain entry per position index.
            std::vector<int32_t>  m_next;      ///< Next entry in the chain, or -1.
            std::vector<int32_t>  m_normal;
            std::vector<int32_t>  m_texCoord;
            std::vector<uint32_t> m_vertex;    ///< Index into Mesh::vertices.
        };

        // ─────────────────────────────────────────────────────────────────────
        // Shared post-processing
        // ─────────────────────────────────────────────────────────────────────

        /**
         * @brief Generates smooth normals by area-weighted averaging of face normals.
         *
         * The cross product of two triangle edges has a magnitude proportional to twice the
         * triangle's area, so *not* normalising it before accumulation weights each face by its
         * area automatically. That is what stops a dense cluster of tiny triangles from
         * outvoting one large neighbouring face and denting the shading.
         */
        void generateSmoothNormals(std::vector<MeshVertex>& vertices,
                                   const std::vector<uint32_t>& indices)
        {
            std::vector<float> accumulated(vertices.size() * 3, 0.0f);

            for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                const MeshVertex& a = vertices[indices[i + 0]];
                const MeshVertex& b = vertices[indices[i + 1]];
                const MeshVertex& c = vertices[indices[i + 2]];

                const float e1[3] = { b.position[0] - a.position[0],
                                      b.position[1] - a.position[1],
                                      b.position[2] - a.position[2] };
                const float e2[3] = { c.position[0] - a.position[0],
                                      c.position[1] - a.position[1],
                                      c.position[2] - a.position[2] };

                const float faceNormal[3] = {
                    e1[1] * e2[2] - e1[2] * e2[1],
                    e1[2] * e2[0] - e1[0] * e2[2],
                    e1[0] * e2[1] - e1[1] * e2[0]
                };

                for (int corner = 0; corner < 3; ++corner) {
                    const size_t base = static_cast<size_t>(indices[i + corner]) * 3;
                    accumulated[base + 0] += faceNormal[0];
                    accumulated[base + 1] += faceNormal[1];
                    accumulated[base + 2] += faceNormal[2];
                }
            }

            for (size_t v = 0; v < vertices.size(); ++v) {
                const float x = accumulated[v * 3 + 0];
                const float y = accumulated[v * 3 + 1];
                const float z = accumulated[v * 3 + 2];
                if (x * x + y * y + z * z > 1e-24f) {
                    vertices[v].packedNormal = packNormal(x, y, z);
                } else {
                    // Degenerate or unreferenced vertex. Any unit vector beats a zero normal,
                    // which would light the surface black.
                    vertices[v].packedNormal = packNormal(0.0f, 1.0f, 0.0f);
                }
            }
        }

        void computeBounds(Mesh& mesh)
        {
            for (MeshSubset& subset : mesh.subsets) {
                for (uint32_t i = 0; i < subset.indexCount; ++i) {
                    const MeshVertex& vertex = mesh.vertices[mesh.indices[subset.firstIndex + i]];
                    for (int axis = 0; axis < 3; ++axis) {
                        subset.boundsMin[axis] = std::min(subset.boundsMin[axis], vertex.position[axis]);
                        subset.boundsMax[axis] = std::max(subset.boundsMax[axis], vertex.position[axis]);
                    }
                }
                if (subset.indexCount == 0) continue;
                for (int axis = 0; axis < 3; ++axis) {
                    mesh.boundsMin[axis] = std::min(mesh.boundsMin[axis], subset.boundsMin[axis]);
                    mesh.boundsMax[axis] = std::max(mesh.boundsMax[axis], subset.boundsMax[axis]);
                }
            }
        }

        /// @brief MTL paths are often authored on Windows and contain backslashes.
        std::filesystem::path resolveTexture(const std::filesystem::path& baseDirectory,
                                             const std::string& name)
        {
            if (name.empty()) return {};
            std::string fixed = name;
            std::replace(fixed.begin(), fixed.end(), '\\', '/');
            while (!fixed.empty() && (fixed.front() == '"' || fixed.front() == ' ')) {
                fixed.erase(fixed.begin());
            }
            while (!fixed.empty() && (fixed.back() == '"' || fixed.back() == ' ')) fixed.pop_back();
            if (fixed.empty()) return {};
            return baseDirectory / fixed;
        }

        // ─────────────────────────────────────────────────────────────────────
        // OBJ
        // ─────────────────────────────────────────────────────────────────────

        bool loadObj(const std::filesystem::path& path, Mesh& mesh, std::string& error)
        {
            tinyobj::ObjReaderConfig config;
            // Materials are referenced by a bare filename inside the .obj, so the search path is
            // the model's own directory rather than the working directory.
            config.mtl_search_path = path.parent_path().string();
            config.triangulate = true;

            tinyobj::ObjReader reader;
            if (!reader.ParseFromFile(path.string(), config)) {
                error = reader.Error().empty() ? "failed to parse OBJ" : reader.Error();
                return false;
            }

            const tinyobj::attrib_t& attrib = reader.GetAttrib();
            const std::vector<tinyobj::shape_t>& shapes = reader.GetShapes();
            const std::vector<tinyobj::material_t>& objMaterials = reader.GetMaterials();

            mesh.hadNormals = !attrib.normals.empty();
            mesh.hadTexCoords = !attrib.texcoords.empty();

            for (const tinyobj::material_t& source : objMaterials) {
                MeshMaterial material;
                material.name = source.name;

                material.baseColor[0] = source.diffuse[0];
                material.baseColor[1] = source.diffuse[1];
                material.baseColor[2] = source.diffuse[2];
                material.emissive[0]  = source.emission[0];
                material.emissive[1]  = source.emission[1];
                material.emissive[2]  = source.emission[2];
                material.opacity      = source.dissolve;

                // Ns is a Phong exponent between 0 and 1000. This is the usual conversion to a
                // perceptual roughness. Exact agreement is impossible — the two models describe
                // different things — but the ordering is preserved, which is what matters.
                material.roughness = std::sqrt(2.0f / (std::max(source.shininess, 0.0f) + 2.0f));
                material.roughness = std::max(0.045f, std::min(1.0f, material.roughness));

                // MTL has no metalness. A bright, near-white specular colour together with a high
                // exponent is how a metal was expressed before physically based workflows, so
                // that is what is looked for. Deliberately conservative: a false positive turns
                // plaster into chrome, which is far more visible than a missed metal.
                const float specularLevel =
                    (source.specular[0] + source.specular[1] + source.specular[2]) / 3.0f;
                material.metallic = (specularLevel > 0.85f && source.shininess > 200.0f) ? 1.0f : 0.0f;

                material.albedoTexture = resolveTexture(mesh.baseDirectory, source.diffuse_texname);
                material.alphaTexture  = resolveTexture(mesh.baseDirectory, source.alpha_texname);
                material.normalTexture = resolveTexture(mesh.baseDirectory, source.bump_texname);
                if (material.normalTexture.empty()) {
                    material.normalTexture = resolveTexture(mesh.baseDirectory, source.normal_texname);
                }

                // A first classification from what the file says outright. Materials whose mask
                // lives in the albedo image's alpha channel — which is how San Miguel's foliage
                // is authored, since its MTL has no map_d at all — cannot be detected here: it
                // takes looking at the pixels, so the viewer refines this after loading textures.
                if (!material.alphaTexture.empty()) {
                    material.blendMode = MaterialBlendMode::Cutout;
                } else if (material.opacity < 0.999f) {
                    material.blendMode = MaterialBlendMode::Transparent;
                }
                material.twoSided = (material.blendMode == MaterialBlendMode::Cutout);

                mesh.materials.push_back(std::move(material));
            }

            const size_t positionCount = attrib.vertices.size() / 3;
            VertexDeduplicator dedup(positionCount);
            // Most positions yield one vertex; seams and hard edges add a few more.
            dedup.reserve(positionCount + positionCount / 2);
            mesh.vertices.reserve(positionCount + positionCount / 2);

            size_t totalIndices = 0;
            for (const tinyobj::shape_t& shape : shapes) totalIndices += shape.mesh.indices.size();
            mesh.indices.reserve(totalIndices);

            for (const tinyobj::shape_t& shape : shapes) {
                // Faces sharing a material become one subset. Sorting by material would produce
                // fewer subsets; preserving file order keeps each subset spatially coherent,
                // which matters more once they are culled individually.
                int currentMaterial = -2;
                MeshSubset subset;

                const size_t faceCount = shape.mesh.indices.size() / 3;
                for (size_t face = 0; face < faceCount; ++face) {
                    const int faceMaterial = face < shape.mesh.material_ids.size()
                        ? shape.mesh.material_ids[face] : -1;

                    if (faceMaterial != currentMaterial) {
                        if (subset.indexCount > 0) mesh.subsets.push_back(subset);
                        subset = MeshSubset{};
                        subset.firstIndex = static_cast<uint32_t>(mesh.indices.size());
                        subset.materialIndex = faceMaterial;
                        currentMaterial = faceMaterial;
                    }

                    for (int corner = 0; corner < 3; ++corner) {
                        const tinyobj::index_t& source = shape.mesh.indices[face * 3 + corner];

                        const uint32_t index = dedup.resolve(
                            source.vertex_index, source.normal_index, source.texcoord_index,
                            [&]() -> uint32_t {
                                MeshVertex vertex{};
                                if (source.vertex_index >= 0) {
                                    const size_t base = static_cast<size_t>(source.vertex_index) * 3;
                                    vertex.position[0] = attrib.vertices[base + 0];
                                    vertex.position[1] = attrib.vertices[base + 1];
                                    vertex.position[2] = attrib.vertices[base + 2];
                                }
                                if (source.normal_index >= 0) {
                                    const size_t base = static_cast<size_t>(source.normal_index) * 3;
                                    vertex.packedNormal = packNormal(attrib.normals[base + 0],
                                                                     attrib.normals[base + 1],
                                                                     attrib.normals[base + 2]);
                                }
                                if (source.texcoord_index >= 0) {
                                    const size_t base = static_cast<size_t>(source.texcoord_index) * 2;
                                    vertex.uv[0] = attrib.texcoords[base + 0];
                                    // OBJ's V axis points up, texture space points down.
                                    vertex.uv[1] = 1.0f - attrib.texcoords[base + 1];
                                }
                                mesh.vertices.push_back(vertex);
                                return static_cast<uint32_t>(mesh.vertices.size() - 1);
                            });

                        mesh.indices.push_back(index);
                        ++subset.indexCount;
                    }
                }
                if (subset.indexCount > 0) mesh.subsets.push_back(subset);
            }

            if (!mesh.hadNormals) generateSmoothNormals(mesh.vertices, mesh.indices);

            mesh.sourceFormat = "OBJ";
            return true;
        }

        // ─────────────────────────────────────────────────────────────────────
        // STL
        // ─────────────────────────────────────────────────────────────────────

        bool loadStl(const std::filesystem::path& path, Mesh& mesh, std::string& error)
        {
            microstl::MeshReaderHandler handler;
            const microstl::Result result = microstl::Reader::readStlFile(path.string(), handler);
            if (result != microstl::Result::Success) {
                error = "failed to read STL: " + std::string(microstl::getResultString(result));
                return false;
            }

            const microstl::Mesh& source = handler.mesh;
            mesh.vertices.reserve(source.facets.size() * 3);
            mesh.indices.reserve(source.facets.size() * 3);

            // STL repeats every vertex per facet. Welding by exact position bytes recovers the
            // sharing, which is what makes the post-transform cache useful at all.
            std::unordered_map<uint64_t, uint32_t> welded;
            welded.reserve(source.facets.size() * 2);

            auto positionKey = [](const microstl::Vertex& v) {
                uint64_t hash = 1469598103934665603ull;
                const float components[3] = { v.x, v.y, v.z };
                for (float component : components) {
                    uint32_t bits;
                    std::memcpy(&bits, &component, sizeof(bits));
                    hash ^= bits;
                    hash *= 1099511628211ull;
                }
                return hash;
            };

            for (const microstl::Facet& facet : source.facets) {
                const microstl::Vertex* corners[3] = { &facet.v1, &facet.v2, &facet.v3 };
                for (const microstl::Vertex* corner : corners) {
                    const uint64_t hashKey = positionKey(*corner);
                    auto it = welded.find(hashKey);
                    if (it == welded.end()) {
                        MeshVertex vertex{};
                        vertex.position[0] = corner->x;
                        vertex.position[1] = corner->y;
                        vertex.position[2] = corner->z;
                        mesh.vertices.push_back(vertex);
                        it = welded.emplace(hashKey,
                                            static_cast<uint32_t>(mesh.vertices.size() - 1)).first;
                    }
                    mesh.indices.push_back(it->second);
                }
            }

            // STL stores a normal per facet, not per vertex. Smoothing across the welded mesh
            // gives a better result than replicating the facet normal, and matches what every
            // other viewer does with these files.
            generateSmoothNormals(mesh.vertices, mesh.indices);

            MeshSubset subset;
            subset.firstIndex = 0;
            subset.indexCount = static_cast<uint32_t>(mesh.indices.size());
            mesh.subsets.push_back(subset);

            mesh.sourceFormat = "STL";
            return true;
        }

        // ─────────────────────────────────────────────────────────────────────
        // PLY
        // ─────────────────────────────────────────────────────────────────────

        bool loadPly(const std::filesystem::path& path, Mesh& mesh, std::string& error)
        {
            try {
                happly::PLYData ply(path.string());

                const std::vector<std::array<double, 3>> positions = ply.getVertexPositions();
                const std::vector<std::vector<size_t>> faces = ply.getFaceIndices<size_t>();

                mesh.vertices.reserve(positions.size());
                for (const std::array<double, 3>& position : positions) {
                    MeshVertex vertex{};
                    vertex.position[0] = static_cast<float>(position[0]);
                    vertex.position[1] = static_cast<float>(position[1]);
                    vertex.position[2] = static_cast<float>(position[2]);
                    mesh.vertices.push_back(vertex);
                }

                // PLY faces may be arbitrary polygons. A triangle fan is correct for convex
                // faces, which is what exporters produce in practice.
                for (const std::vector<size_t>& face : faces) {
                    for (size_t corner = 2; corner < face.size(); ++corner) {
                        mesh.indices.push_back(static_cast<uint32_t>(face[0]));
                        mesh.indices.push_back(static_cast<uint32_t>(face[corner - 1]));
                        mesh.indices.push_back(static_cast<uint32_t>(face[corner]));
                    }
                }

                generateSmoothNormals(mesh.vertices, mesh.indices);

                MeshSubset subset;
                subset.firstIndex = 0;
                subset.indexCount = static_cast<uint32_t>(mesh.indices.size());
                mesh.subsets.push_back(subset);

                mesh.sourceFormat = "PLY";
                return true;
            } catch (const std::exception& e) {
                error = std::string("failed to read PLY: ") + e.what();
                return false;
            }
        }

    } // namespace

    float MeshSubset::radius() const
    {
        const float dx = (boundsMax[0] - boundsMin[0]) * 0.5f;
        const float dy = (boundsMax[1] - boundsMin[1]) * 0.5f;
        const float dz = (boundsMax[2] - boundsMin[2]) * 0.5f;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    float Mesh::boundsExtent() const
    {
        return std::max({ boundsMax[0] - boundsMin[0],
                          boundsMax[1] - boundsMin[1],
                          boundsMax[2] - boundsMin[2] });
    }

    Mesh loadMesh(const std::filesystem::path& path, std::string& error)
    {
        Mesh mesh;

        if (!std::filesystem::exists(path)) {
            error = "file not found: " + path.string();
            return mesh;
        }
        mesh.baseDirectory = path.parent_path();

        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        bool ok = false;
        if (extension == ".obj")      ok = loadObj(path, mesh, error);
        else if (extension == ".stl") ok = loadStl(path, mesh, error);
        else if (extension == ".ply") ok = loadPly(path, mesh, error);
        else {
            error = "unsupported extension: " + extension + " (expected .obj, .stl or .ply)";
            return mesh;
        }

        if (!ok) { mesh = Mesh{}; return mesh; }

        if (mesh.indices.empty()) {
            error = "file parsed but contains no triangles";
            mesh = Mesh{};
            return mesh;
        }

        computeBounds(mesh);
        return mesh;
    }

} // namespace dmrender
