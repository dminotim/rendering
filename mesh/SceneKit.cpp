//
// Assembling a scene out of an asset kit.
//
// A kit is not a scene. Packs like Megascans' interior sets ship a folder per asset — one FBX and
// its texture maps — and every one of them sits at the origin with an identity transform, because
// the placement is expected to happen in whatever editor the artist uses. Nothing in the download
// records where anything goes.
//
// So the layout has to live somewhere, and this file makes it a small text format that sits beside
// the kit and is meant to be edited by hand:
//
//     kit    ../saloon_interior_high        # folder holding <assetId>/<assetId>.fbx
//     place  wbhobeedw   -2.0 0 -3.0   90        # id, position in metres, yaw in degrees
//     place  ukknbeyaw    1.2 0  0.4  -35  0.9   # optional uniform scale
//     floor  wldocam     12 10  0.0     4        # material, extent, height, texture repeats
//
// Text rather than a binary or a header of constants for one reason: iterating on a layout means
// moving a chair four times, and a format that needs a recompile to move a chair does not get
// iterated on. The result is merged into a single Mesh — one vertex buffer, one subset per
// placement — because that is the shape the renderer already culls, sorts and draws well.
//

#include "Mesh.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <unordered_map>

namespace dmrender {

    namespace {

        /**
         * @brief A placement transform as a 3x4 affine matrix, column-major.
         *
         * `m[column][row]`, with column 3 the translation. General rather than
         * position-plus-yaw because layouts exported from an editor are general: a plank leaning
         * against a wall, a saddle over a rail and a mirror tilted off the vertical all carry
         * rotations that no amount of yaw will express, and quietly dropping them turns a dressed
         * room into a room where a dozen things lie flat on the floor.
         */
        struct Transform {
            float m[4][3] = { {1,0,0}, {0,1,0}, {0,0,1}, {0,0,0} };

            void point(const float in[3], float out[3]) const
            {
                for (int row = 0; row < 3; ++row) {
                    out[row] = m[0][row] * in[0] + m[1][row] * in[1] + m[2][row] * in[2] + m[3][row];
                }
            }
        };

        Transform fromYaw(const float position[3], float yawDegrees, float scale)
        {
            const float yaw = yawDegrees * 3.14159265358979323846f / 180.0f;
            const float c = std::cos(yaw) * scale;
            const float s = std::sin(yaw) * scale;
            Transform t;
            t.m[0][0] =  c; t.m[0][1] = 0.0f;  t.m[0][2] = -s;
            t.m[1][0] = 0.0f; t.m[1][1] = scale; t.m[1][2] = 0.0f;
            t.m[2][0] =  s; t.m[2][1] = 0.0f;  t.m[2][2] =  c;
            t.m[3][0] = position[0]; t.m[3][1] = position[1]; t.m[3][2] = position[2];
            return t;
        }

        /**
         * @brief Inverse transpose of the linear part, for transforming normals.
         *
         * Under non-uniform scale a normal rotated like a position stops being perpendicular to
         * its surface — a squashed sphere shades like a sphere. Layouts from an editor do contain
         * non-uniform scale (walls stretched to fit, planks lengthened), so this is not a
         * theoretical concern. Falls back to the linear part itself when it is singular, which is
         * the best available answer for a degenerate transform.
         */
        void normalMatrix(const Transform& t, float out[3][3])
        {
            const float a = t.m[0][0], b = t.m[1][0], c = t.m[2][0];
            const float d = t.m[0][1], e = t.m[1][1], f = t.m[2][1];
            const float g = t.m[0][2], h = t.m[1][2], i = t.m[2][2];

            const float determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
            if (std::abs(determinant) < 1e-12f) {
                out[0][0] = a; out[1][0] = b; out[2][0] = c;
                out[0][1] = d; out[1][1] = e; out[2][1] = f;
                out[0][2] = g; out[1][2] = h; out[2][2] = i;
                return;
            }
            const float inverse = 1.0f / determinant;
            // Transpose of the inverse, written directly from the cofactors.
            out[0][0] = (e * i - f * h) * inverse;
            out[1][0] = (f * g - d * i) * inverse;
            out[2][0] = (d * h - e * g) * inverse;
            out[0][1] = (c * h - b * i) * inverse;
            out[1][1] = (a * i - c * g) * inverse;
            out[2][1] = (b * g - a * h) * inverse;
            out[0][2] = (b * f - c * e) * inverse;
            out[1][2] = (c * d - a * f) * inverse;
            out[2][2] = (a * e - b * d) * inverse;
        }

        /// @brief Appends @p source into @p target under @p transform.
        void appendTransformed(Mesh& target, const Mesh& source, const Transform& transform,
                               int32_t materialIndex)
        {
            float normals[3][3];
            normalMatrix(transform, normals);

            // A transform that mirrors — negative determinant, which an exported layout uses to
            // reuse one prop as its own mirror image — reverses triangle winding, so the faces
            // have to be re-ordered or the whole prop is backface-culled away.
            const float determinant =
                  transform.m[0][0] * (transform.m[1][1] * transform.m[2][2] - transform.m[2][1] * transform.m[1][2])
                - transform.m[1][0] * (transform.m[0][1] * transform.m[2][2] - transform.m[2][1] * transform.m[0][2])
                + transform.m[2][0] * (transform.m[0][1] * transform.m[1][2] - transform.m[1][1] * transform.m[0][2]);
            const bool mirrored = determinant < 0.0f;

            const uint32_t vertexBase = static_cast<uint32_t>(target.vertices.size());
            const uint32_t firstIndex = static_cast<uint32_t>(target.indices.size());

            target.vertices.reserve(target.vertices.size() + source.vertices.size());
            for (const MeshVertex& in : source.vertices) {
                MeshVertex out = in;
                transform.point(in.position, out.position);

                // The normal is packed, so transforming it means unpacking and repacking.
                float normal[3];
                unpackNormal(in.packedNormal, normal);
                float rotated[3];
                for (int row = 0; row < 3; ++row) {
                    rotated[row] = normals[0][row] * normal[0]
                                 + normals[1][row] * normal[1]
                                 + normals[2][row] * normal[2];
                }
                out.packedNormal = packNormal(rotated[0], rotated[1], rotated[2]);
                target.vertices.push_back(out);
            }

            target.indices.reserve(target.indices.size() + source.indices.size());
            for (size_t i = 0; i + 2 < source.indices.size(); i += 3) {
                const uint32_t a = vertexBase + source.indices[i];
                const uint32_t b = vertexBase + source.indices[i + 1];
                const uint32_t c = vertexBase + source.indices[i + 2];
                target.indices.push_back(a);
                target.indices.push_back(mirrored ? c : b);
                target.indices.push_back(mirrored ? b : c);
            }

            MeshSubset subset;
            subset.firstIndex = firstIndex;
            subset.indexCount = static_cast<uint32_t>(source.indices.size());
            subset.materialIndex = materialIndex;
            target.subsets.push_back(subset);
        }

        /**
         * @brief Adds a horizontal quad, tiled, using an existing material.
         *
         * The kits ship walls and props but no ground: an interior set assumes the floor comes
         * from the level. Without one every shadow falls into the void and the scene reads as
         * furniture floating in fog, so the format can synthesise one from any asset's material.
         */
        void appendFloor(Mesh& target, float extentX, float extentZ, float height,
                         float repeats, int32_t materialIndex)
        {
            const uint32_t vertexBase = static_cast<uint32_t>(target.vertices.size());
            const uint32_t firstIndex = static_cast<uint32_t>(target.indices.size());
            const uint32_t up = packNormal(0.0f, 1.0f, 0.0f);

            const float halfX = extentX * 0.5f;
            const float halfZ = extentZ * 0.5f;
            const float corners[4][2] = { { -halfX, -halfZ }, { halfX, -halfZ },
                                          {  halfX,  halfZ }, { -halfX,  halfZ } };
            const float uvs[4][2] = { { 0, 0 }, { repeats, 0 }, { repeats, repeats }, { 0, repeats } };

            for (int corner = 0; corner < 4; ++corner) {
                MeshVertex vertex{};
                vertex.position[0] = corners[corner][0];
                vertex.position[1] = height;
                vertex.position[2] = corners[corner][1];
                vertex.packedNormal = up;
                vertex.uv[0] = uvs[corner][0];
                vertex.uv[1] = uvs[corner][1];
                target.vertices.push_back(vertex);
            }
            const uint32_t order[6] = { 0, 2, 1, 0, 3, 2 };
            for (uint32_t i : order) target.indices.push_back(vertexBase + i);

            MeshSubset subset;
            subset.firstIndex = firstIndex;
            subset.indexCount = 6;
            subset.materialIndex = materialIndex;
            target.subsets.push_back(subset);
        }

    } // namespace

    /**
     * @brief Loads a `.dmscene` layout and the kit assets it references.
     *
     * Each distinct asset is read from disk once however many times it is placed; the geometry is
     * then copied per placement. Copying rather than instancing is a deliberate trade: the
     * renderer draws from one vertex buffer with per-subset culling, and an interior of a few
     * hundred props costs a few million triangles — well inside what the geometry path already
     * handles, and it keeps every existing code path working unchanged.
     */
    bool loadScene(const std::filesystem::path& path, Mesh& mesh, std::string& error)
    {
        std::ifstream file(path);
        if (!file) { error = "cannot open " + path.string(); return false; }

        const std::filesystem::path sceneDirectory = path.parent_path();
        std::filesystem::path kitRoot = sceneDirectory;

        // Each asset is parsed once and kept, so a chair placed twenty times costs one read.
        std::unordered_map<std::string, Mesh> loaded;
        std::unordered_map<std::string, int32_t> materialOf;

        auto resolveAsset = [&](const std::string& assetId, std::string& why) -> const Mesh* {
            if (const auto found = loaded.find(assetId); found != loaded.end()) {
                return found->second.empty() ? nullptr : &found->second;
            }

            // Three spellings, because two kinds of layout reference assets two ways. A kit nests
            // as <root>/<id>/<id>.fbx and is named by id; a layout exported from an editor names a
            // path inside a project tree, which arrives here already carrying its own .fbx.
            std::filesystem::path assetPath;
            const bool looksLikePath = assetId.find('/') != std::string::npos
                                    || assetId.find('\\') != std::string::npos
                                    || assetId.size() > 4
                                       && assetId.compare(assetId.size() - 4, 4, ".fbx") == 0;
            if (looksLikePath) {
                assetPath = std::filesystem::path(assetId);
                if (assetPath.is_relative()) assetPath = kitRoot / assetPath;
            } else {
                assetPath = kitRoot / assetId / (assetId + ".fbx");
                if (!std::filesystem::exists(assetPath)) assetPath = kitRoot / (assetId + ".fbx");
            }
            if (!std::filesystem::exists(assetPath)) {
                why = "asset not found: " + assetId;
                std::fprintf(stderr, "  scene: not found %s\n", assetPath.string().c_str());
                loaded.emplace(assetId, Mesh{});
                return nullptr;
            }

            Mesh asset;
            asset.baseDirectory = assetPath.parent_path();
            if (!loadFbx(assetPath, asset, why)) {
                // Reported per distinct asset rather than per placement: a converted layout can
                // reference one broken mesh from two hundred lines, and a silent skip means the
                // scene simply comes up missing a wall with nothing to say why.
                std::fprintf(stderr, "  scene: cannot load %s: %s\n",
                             assetPath.filename().string().c_str(), why.c_str());
                loaded.emplace(assetId, Mesh{});
                return nullptr;
            }

            const auto inserted = loaded.emplace(assetId, std::move(asset));

            // The kit gives each asset exactly one material; it enters the scene once and every
            // placement of that asset shares it, which is what keeps the material count equal to
            // the number of distinct assets rather than the number of props.
            MeshMaterial material = inserted.first->second.materials.empty()
                                  ? MeshMaterial{}
                                  : inserted.first->second.materials[0];
            material.name = assetId;
            materialOf[assetId] = static_cast<int32_t>(mesh.materials.size());
            mesh.materials.push_back(std::move(material));

            return &inserted.first->second;
        };

        std::string line;
        int lineNumber = 0;
        int placed = 0;
        std::string firstFailure;

        while (std::getline(file, line)) {
            ++lineNumber;
            if (const size_t comment = line.find('#'); comment != std::string::npos) {
                line.erase(comment);
            }
            std::istringstream stream(line);
            std::string command;
            if (!(stream >> command)) continue;

            if (command == "kit") {
                std::string folder;
                stream >> std::ws;
                std::getline(stream, folder);
                while (!folder.empty() && std::isspace(static_cast<unsigned char>(folder.back()))) {
                    folder.pop_back();
                }
                if (folder.empty()) continue;
                std::filesystem::path candidate(folder);
                kitRoot = candidate.is_absolute() ? candidate : sceneDirectory / candidate;
                continue;
            }

            if (command == "place") {
                std::string assetId;
                float position[3] = { 0.0f, 0.0f, 0.0f };
                float yawDegrees = 0.0f;
                float scale = 1.0f;
                if (!(stream >> assetId >> position[0] >> position[1] >> position[2])) {
                    error = "malformed place on line " + std::to_string(lineNumber);
                    return false;
                }
                stream >> yawDegrees;   // optional
                stream >> scale;        // optional
                if (scale <= 0.0f) scale = 1.0f;

                std::string why;
                const Mesh* asset = resolveAsset(assetId, why);
                if (!asset) {
                    // One missing prop should not lose the other two hundred; the layout is
                    // hand-written and a typo in it is the expected failure, not a fatal one.
                    if (firstFailure.empty()) firstFailure = why;
                    continue;
                }
                appendTransformed(mesh, *asset, fromYaw(position, yawDegrees, scale),
                                  materialOf[assetId]);
                ++placed;
                continue;
            }

            // An exact placement, as exported from an editor: the asset path followed by a 3x4
            // affine matrix in column-major order (three basis columns, then the translation).
            // Hand-authored layouts use `place`; this is what a converted scene emits, because a
            // real layout has rotations and scales that position-and-yaw cannot represent.
            if (command == "instance") {
                // Split from the right, not the left: the path may contain spaces — a project
                // directory called "HDRP (Default)" is enough to break naive tokenising — while
                // the matrix is always exactly twelve trailing numbers. Anything before them is
                // the path, spaces and all, so no quoting rules are needed.
                std::vector<std::string> tokens;
                for (std::string token; stream >> token; ) tokens.push_back(std::move(token));
                if (tokens.size() < 13) {
                    error = "instance needs a path and 12 matrix values, line "
                          + std::to_string(lineNumber);
                    return false;
                }

                const size_t matrixStart = tokens.size() - 12;
                std::string assetId = tokens[0];
                for (size_t i = 1; i < matrixStart; ++i) assetId += " " + tokens[i];

                Transform transform;
                for (int value = 0; value < 12; ++value) {
                    try {
                        transform.m[value / 3][value % 3] =
                                std::stof(tokens[matrixStart + static_cast<size_t>(value)]);
                    } catch (const std::exception&) {
                        error = "instance matrix value is not a number, line "
                              + std::to_string(lineNumber);
                        return false;
                    }
                }

                std::string why;
                const Mesh* asset = resolveAsset(assetId, why);
                if (!asset) {
                    if (firstFailure.empty()) firstFailure = why;
                    continue;
                }
                appendTransformed(mesh, *asset, transform, materialOf[assetId]);
                ++placed;
                continue;
            }

            if (command == "floor") {
                std::string assetId;
                float extentX = 10.0f, extentZ = 10.0f, height = 0.0f, repeats = 4.0f;
                if (!(stream >> assetId >> extentX >> extentZ)) {
                    error = "malformed floor on line " + std::to_string(lineNumber);
                    return false;
                }
                stream >> height;
                stream >> repeats;
                if (repeats <= 0.0f) repeats = 1.0f;

                std::string why;
                if (!resolveAsset(assetId, why)) {
                    if (firstFailure.empty()) firstFailure = why;
                    continue;
                }
                appendFloor(mesh, extentX, extentZ, height, repeats, materialOf[assetId]);
                ++placed;
                continue;
            }

            error = "unknown command '" + command + "' on line " + std::to_string(lineNumber);
            return false;
        }

        if (placed == 0) {
            error = firstFailure.empty() ? "scene file placed nothing" : firstFailure;
            return false;
        }

        mesh.hadNormals   = true;
        mesh.hadTexCoords = true;
        mesh.sourceFormat = "kit scene (" + std::to_string(placed) + " placements, "
                          + std::to_string(mesh.materials.size()) + " assets)";
        mesh.baseDirectory = sceneDirectory;
        return true;
    }

} // namespace dmrender
