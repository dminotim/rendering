#include "Mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include "happly.h"
#include "microstl.h"

namespace dmrender {

    namespace {

        // ─────────────────────────────────────────────────────────────────────
        // Shared post-processing
        // ─────────────────────────────────────────────────────────────────────

        /**
         * @brief Generates smooth normals by area-weighted averaging of face normals.
         *
         * Used for every format that does not carry normals — which includes most STL-adjacent
         * pipelines and, notably, this project's bunny.obj.
         *
         * The cross product of two triangle edges has a magnitude proportional to twice the
         * triangle's area, so *not* normalising it before accumulation weights each face by its
         * area automatically. That is what stops a dense cluster of tiny triangles from
         * outvoting one large neighbouring face and denting the shading.
         */
        void generateSmoothNormals(std::vector<MeshVertex>& vertices,
                                   const std::vector<uint32_t>& indices)
        {
            for (MeshVertex& vertex : vertices) {
                vertex.normal[0] = vertex.normal[1] = vertex.normal[2] = 0.0f;
            }

            for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                MeshVertex& a = vertices[indices[i + 0]];
                MeshVertex& b = vertices[indices[i + 1]];
                MeshVertex& c = vertices[indices[i + 2]];

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

                for (MeshVertex* vertex : { &a, &b, &c }) {
                    vertex->normal[0] += faceNormal[0];
                    vertex->normal[1] += faceNormal[1];
                    vertex->normal[2] += faceNormal[2];
                }
            }

            for (MeshVertex& vertex : vertices) {
                const float length = std::sqrt(vertex.normal[0] * vertex.normal[0] +
                                               vertex.normal[1] * vertex.normal[1] +
                                               vertex.normal[2] * vertex.normal[2]);
                if (length > 1e-12f) {
                    vertex.normal[0] /= length;
                    vertex.normal[1] /= length;
                    vertex.normal[2] /= length;
                } else {
                    // Degenerate or unreferenced vertex. Any unit vector beats a zero normal,
                    // which would light the surface black.
                    vertex.normal[0] = 0.0f;
                    vertex.normal[1] = 1.0f;
                    vertex.normal[2] = 0.0f;
                }
            }
        }

        void computeBounds(Mesh& mesh)
        {
            if (mesh.vertices.empty()) return;

            for (int axis = 0; axis < 3; ++axis) {
                mesh.boundsMin[axis] = mesh.vertices[0].position[axis];
                mesh.boundsMax[axis] = mesh.vertices[0].position[axis];
            }
            for (const MeshVertex& vertex : mesh.vertices) {
                for (int axis = 0; axis < 3; ++axis) {
                    mesh.boundsMin[axis] = std::min(mesh.boundsMin[axis], vertex.position[axis]);
                    mesh.boundsMax[axis] = std::max(mesh.boundsMax[axis], vertex.position[axis]);
                }
            }
        }

        /// @brief Key for collapsing identical vertices into one index.
        struct VertexKey {
            int position = -1;
            int normal = -1;
            int texCoord = -1;

            bool operator==(const VertexKey& other) const {
                return position == other.position && normal == other.normal &&
                       texCoord == other.texCoord;
            }
        };

        struct VertexKeyHash {
            size_t operator()(const VertexKey& key) const {
                size_t hash = 1469598103934665603ull;
                for (int value : { key.position, key.normal, key.texCoord }) {
                    hash ^= static_cast<size_t>(value + 1);
                    hash *= 1099511628211ull;
                }
                return hash;
            }
        };

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
                error = reader.Error().empty() ? "не удалось разобрать OBJ" : reader.Error();
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
                if (!source.diffuse_texname.empty()) {
                    material.albedoTexture = path.parent_path() / source.diffuse_texname;
                }
                mesh.materials.push_back(std::move(material));
            }

            // An OBJ addresses position, normal and texture coordinate with *independent*
            // indices, which no GPU can do — it wants one index per vertex. Collapsing each
            // unique combination into a single vertex is what bridges the two models, and it
            // deduplicates as a side effect.
            std::unordered_map<VertexKey, uint32_t, VertexKeyHash> uniqueVertices;

            for (const tinyobj::shape_t& shape : shapes) {
                // Faces sharing a material become one subset. Sorting by material would produce
                // fewer subsets; preserving file order keeps the mapping obvious instead.
                int currentMaterial = -2;
                MeshSubset subset;

                for (size_t face = 0; face < shape.mesh.indices.size() / 3; ++face) {
                    const int faceMaterial = face < shape.mesh.material_ids.size()
                        ? shape.mesh.material_ids[face] : -1;

                    if (faceMaterial != currentMaterial) {
                        if (currentMaterial != -2 && subset.indexCount > 0) {
                            mesh.subsets.push_back(subset);
                        }
                        subset = MeshSubset{};
                        subset.firstIndex = static_cast<uint32_t>(mesh.indices.size());
                        subset.materialIndex = faceMaterial;
                        currentMaterial = faceMaterial;
                    }

                    for (int corner = 0; corner < 3; ++corner) {
                        const tinyobj::index_t& index = shape.mesh.indices[face * 3 + corner];

                        const VertexKey key{ index.vertex_index, index.normal_index,
                                             index.texcoord_index };
                        auto found = uniqueVertices.find(key);
                        if (found == uniqueVertices.end()) {
                            MeshVertex vertex{};
                            vertex.position[0] = attrib.vertices[3 * index.vertex_index + 0];
                            vertex.position[1] = attrib.vertices[3 * index.vertex_index + 1];
                            vertex.position[2] = attrib.vertices[3 * index.vertex_index + 2];

                            if (index.normal_index >= 0) {
                                vertex.normal[0] = attrib.normals[3 * index.normal_index + 0];
                                vertex.normal[1] = attrib.normals[3 * index.normal_index + 1];
                                vertex.normal[2] = attrib.normals[3 * index.normal_index + 2];
                            }
                            if (index.texcoord_index >= 0) {
                                vertex.uv[0] = attrib.texcoords[2 * index.texcoord_index + 0];
                                // OBJ measures V from the bottom, textures are sampled from the
                                // top. Flipping here keeps every shader free of the question.
                                vertex.uv[1] = 1.0f - attrib.texcoords[2 * index.texcoord_index + 1];
                            }

                            found = uniqueVertices.emplace(
                                key, static_cast<uint32_t>(mesh.vertices.size())).first;
                            mesh.vertices.push_back(vertex);
                        }

                        mesh.indices.push_back(found->second);
                        ++subset.indexCount;
                    }
                }

                if (subset.indexCount > 0) {
                    mesh.subsets.push_back(subset);
                }
            }

            mesh.sourceFormat = "OBJ";
            return true;
        }

        // ─────────────────────────────────────────────────────────────────────
        // STL
        // ─────────────────────────────────────────────────────────────────────

        bool loadStl(const std::filesystem::path& path, Mesh& mesh, std::string& error)
        {
            microstl::MeshReaderHandler handler;
            const microstl::Result result = microstl::Reader::readStlFile(path, handler);
            if (result != microstl::Result::Success) {
                error = "не удалось прочитать STL: " + std::string(microstl::getResultString(result));
                return false;
            }

            // STL has no notion of shared vertices: every triangle repeats its three corners.
            // Welding them by position turns a soup into an indexed mesh, which both shrinks the
            // data and makes smooth normals possible at all.
            std::unordered_map<std::string, uint32_t> welded;
            auto keyOf = [](const microstl::Vertex& v) {
                std::string key(sizeof(float) * 3, '\0');
                std::memcpy(key.data(), &v.x, sizeof(float));
                std::memcpy(key.data() + sizeof(float), &v.y, sizeof(float));
                std::memcpy(key.data() + sizeof(float) * 2, &v.z, sizeof(float));
                return key;
            };

            for (const microstl::Facet& facet : handler.mesh.facets) {
                for (const microstl::Vertex& corner : { facet.v1, facet.v2, facet.v3 }) {
                    const std::string key = keyOf(corner);
                    auto found = welded.find(key);
                    if (found == welded.end()) {
                        MeshVertex vertex{};
                        vertex.position[0] = corner.x;
                        vertex.position[1] = corner.y;
                        vertex.position[2] = corner.z;
                        found = welded.emplace(key, static_cast<uint32_t>(mesh.vertices.size())).first;
                        mesh.vertices.push_back(vertex);
                    }
                    mesh.indices.push_back(found->second);
                }
            }

            // STL stores a per-facet normal, but welding vertices makes it unusable — a shared
            // vertex belongs to several facets. Smooth normals are regenerated below.
            mesh.hadNormals = false;
            mesh.hadTexCoords = false;
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

                // PLY faces may have any number of corners; fan-triangulate them.
                for (const std::vector<size_t>& face : faces) {
                    for (size_t corner = 2; corner < face.size(); ++corner) {
                        mesh.indices.push_back(static_cast<uint32_t>(face[0]));
                        mesh.indices.push_back(static_cast<uint32_t>(face[corner - 1]));
                        mesh.indices.push_back(static_cast<uint32_t>(face[corner]));
                    }
                }
            }
            catch (const std::exception& e) {
                error = std::string("не удалось прочитать PLY: ") + e.what();
                return false;
            }

            mesh.hadNormals = false;
            mesh.hadTexCoords = false;
            mesh.sourceFormat = "PLY";
            return true;
        }

    } // namespace

    Mesh loadMesh(const std::filesystem::path& path, std::string& error)
    {
        Mesh mesh;
        error.clear();

        if (!std::filesystem::exists(path)) {
            error = "файл не найден: " + path.string();
            return mesh;
        }

        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        bool ok = false;
        if (extension == ".obj")      ok = loadObj(path, mesh, error);
        else if (extension == ".stl") ok = loadStl(path, mesh, error);
        else if (extension == ".ply") ok = loadPly(path, mesh, error);
        else {
            error = "неподдерживаемое расширение: " + extension + " (ожидается .obj, .stl или .ply)";
            return mesh;
        }

        if (!ok) {
            return Mesh{};
        }
        if (mesh.empty()) {
            error = "файл разобран, но не содержит треугольников";
            return Mesh{};
        }

        // Every format lands here in the same state: triangulated and indexed. What differs is
        // whether it brought normals, so that is the only thing left to normalise.
        if (!mesh.hadNormals) {
            generateSmoothNormals(mesh.vertices, mesh.indices);
        }

        if (mesh.subsets.empty()) {
            MeshSubset subset;
            subset.firstIndex = 0;
            subset.indexCount = static_cast<uint32_t>(mesh.indices.size());
            subset.materialIndex = -1;
            mesh.subsets.push_back(subset);
        }

        computeBounds(mesh);
        return mesh;
    }

} // namespace dmrender
