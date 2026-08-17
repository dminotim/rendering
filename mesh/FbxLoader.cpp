//
// Binary FBX reading, for the Megascans-style asset kits.
//
// FBX is not a format so much as a container: a tree of named records holding typed properties,
// with the meaning of any particular record decided entirely by convention. This file reads the
// container generically and then interprets exactly the handful of records a renderer needs —
// positions, polygons, normals, texture coordinates, per-polygon material assignment, and the
// node transforms that place a mesh in the world. Everything else in a real FBX file (animation
// takes, deformers, poses, cameras, layered textures) is walked past.
//
// The binary encoding is straightforward once stated:
//
//   header    "Kaydara FBX Binary  \0" + 2 bytes + uint32 version
//   record    endOffset, propertyCount, propertyListLength, nameLength, name,
//             properties..., nested records..., terminated by an all-zero record
//   property  a type byte, then either a scalar, a length-prefixed string, or an array
//
// The one real trap is that the three record offsets are 32-bit up to version 7500 and 64-bit
// from 7500 onwards, with no other change. Getting that wrong reads a valid-looking tree that is
// garbage from the first nested record onward.
//

#include "Mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "stb_image.h"   // for stbi_zlib_decode_buffer; the implementation lives in TextureCache.cpp

namespace dmrender {

    namespace {

        // ─────────────────────────────────────────────────────────────────────
        // The container
        // ─────────────────────────────────────────────────────────────────────

        /**
         * @brief One property of a record, normalised to three storage shapes.
         *
         * FBX has thirteen property types; a reader that kept them apart would spend its whole
         * length in switch statements. Every integer type is widened to int64, every real to
         * double, and arrays to vectors of those — the precision is never lost, and callers can
         * ask for what they want without knowing what the exporter wrote.
         */
        struct FbxProperty {
            char type = '\0';
            int64_t integer = 0;
            double  real = 0.0;
            std::string text;
            std::vector<double>  reals;
            std::vector<int64_t> integers;

            bool isArray() const { return !reals.empty() || !integers.empty(); }
        };

        struct FbxNode {
            std::string name;
            std::vector<FbxProperty> properties;
            std::vector<FbxNode> children;

            const FbxNode* child(const char* wanted) const {
                for (const FbxNode& node : children) {
                    if (node.name == wanted) return &node;
                }
                return nullptr;
            }

            /// @brief First property as a string, or empty. Names and enums live in property 0..2.
            std::string stringAt(size_t index) const {
                return index < properties.size() ? properties[index].text : std::string();
            }
            int64_t integerAt(size_t index, int64_t fallback = 0) const {
                return index < properties.size() ? properties[index].integer : fallback;
            }
        };

        /// @brief Reads the binary encoding described at the top of the file.
        class FbxReader {
        public:
            FbxReader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

            bool parse(FbxNode& root, std::string& error)
            {
                if (m_size < 27) { error = "file is too short to be an FBX"; return false; }
                if (std::memcmp(m_data, "Kaydara FBX Binary  ", 20) != 0) {
                    error = "not a binary FBX (ASCII FBX is not supported)";
                    return false;
                }
                m_position = 23;
                m_version = readScalar<uint32_t>();
                // The width of a record's three offsets changes here and nowhere else.
                m_wideOffsets = m_version >= 7500;

                while (m_position + recordHeaderSize() <= m_size) {
                    FbxNode node;
                    const Status status = readNode(node, error);
                    if (status == Status::Error) return false;
                    if (status == Status::End)   break;
                    root.children.push_back(std::move(node));
                }
                return true;
            }

            uint32_t version() const { return m_version; }

        private:
            enum class Status { Ok, End, Error };

            size_t recordHeaderSize() const { return m_wideOffsets ? 25 : 13; }

            template <typename T> T readScalar()
            {
                T value{};
                if (m_position + sizeof(T) > m_size) { m_position = m_size; return value; }
                std::memcpy(&value, m_data + m_position, sizeof(T));
                m_position += sizeof(T);
                return value;
            }

            uint64_t readOffset()
            {
                return m_wideOffsets ? readScalar<uint64_t>()
                                     : static_cast<uint64_t>(readScalar<uint32_t>());
            }

            Status readNode(FbxNode& node, std::string& error)
            {
                const uint64_t endOffset    = readOffset();
                const uint64_t propertyCount = readOffset();
                readOffset(); // property list length in bytes; the properties are self-describing
                const uint8_t nameLength = readScalar<uint8_t>();

                // An all-zero record closes the current list. It is the only terminator there is.
                if (endOffset == 0) return Status::End;
                if (endOffset > m_size) { error = "record extends past end of file"; return Status::Error; }

                if (m_position + nameLength > m_size) { error = "truncated record name"; return Status::Error; }
                node.name.assign(reinterpret_cast<const char*>(m_data + m_position), nameLength);
                m_position += nameLength;

                node.properties.reserve(static_cast<size_t>(propertyCount));
                for (uint64_t i = 0; i < propertyCount; ++i) {
                    FbxProperty property;
                    if (!readProperty(property, error)) return Status::Error;
                    node.properties.push_back(std::move(property));
                }

                // Anything left before endOffset is nested records. A record with no children
                // still writes the terminator, so the loop below reads it and stops.
                while (m_position + recordHeaderSize() <= endOffset) {
                    FbxNode child;
                    const Status status = readNode(child, error);
                    if (status == Status::Error) return Status::Error;
                    if (status == Status::End)   break;
                    node.children.push_back(std::move(child));
                }

                m_position = static_cast<size_t>(endOffset);
                return Status::Ok;
            }

            /// @brief Expands one array property, inflating it first when it is deflated.
            bool readArray(FbxProperty& property, char type, std::string& error)
            {
                const uint32_t count      = readScalar<uint32_t>();
                const uint32_t encoding   = readScalar<uint32_t>();
                const uint32_t storedSize = readScalar<uint32_t>();

                const size_t elementSize = (type == 'd' || type == 'l') ? 8
                                         : (type == 'b')               ? 1
                                                                       : 4;
                const size_t plainSize = static_cast<size_t>(count) * elementSize;

                if (m_position + storedSize > m_size) { error = "truncated array property"; return false; }
                const uint8_t* source = m_data + m_position;
                m_position += storedSize;

                std::vector<uint8_t> inflated;
                if (encoding == 1) {
                    // Deflate, zlib-wrapped. stb's decoder is already linked for image loading
                    // and handles exactly this, which saves taking a dependency on zlib itself.
                    inflated.resize(plainSize);
                    const int written = stbi_zlib_decode_buffer(
                            reinterpret_cast<char*>(inflated.data()), static_cast<int>(plainSize),
                            reinterpret_cast<const char*>(source), static_cast<int>(storedSize));
                    if (written < 0 || static_cast<size_t>(written) != plainSize) {
                        error = "failed to inflate a compressed array";
                        return false;
                    }
                    source = inflated.data();
                } else if (plainSize > storedSize) {
                    error = "array claims more elements than it stores";
                    return false;
                }

                switch (type) {
                    case 'f': {
                        property.reals.resize(count);
                        for (uint32_t i = 0; i < count; ++i) {
                            float value; std::memcpy(&value, source + i * 4, 4);
                            property.reals[i] = value;
                        }
                        break;
                    }
                    case 'd': {
                        property.reals.resize(count);
                        for (uint32_t i = 0; i < count; ++i) {
                            double value; std::memcpy(&value, source + i * 8, 8);
                            property.reals[i] = value;
                        }
                        break;
                    }
                    case 'i': {
                        property.integers.resize(count);
                        for (uint32_t i = 0; i < count; ++i) {
                            int32_t value; std::memcpy(&value, source + i * 4, 4);
                            property.integers[i] = value;
                        }
                        break;
                    }
                    case 'l': {
                        property.integers.resize(count);
                        for (uint32_t i = 0; i < count; ++i) {
                            int64_t value; std::memcpy(&value, source + i * 8, 8);
                            property.integers[i] = value;
                        }
                        break;
                    }
                    case 'b': {
                        property.integers.resize(count);
                        for (uint32_t i = 0; i < count; ++i) property.integers[i] = source[i];
                        break;
                    }
                    default: error = "unknown array type"; return false;
                }
                return true;
            }

            bool readProperty(FbxProperty& property, std::string& error)
            {
                if (m_position >= m_size) { error = "truncated property"; return false; }
                const char type = static_cast<char>(m_data[m_position++]);
                property.type = type;

                switch (type) {
                    case 'Y': property.integer = readScalar<int16_t>(); return true;
                    case 'C': property.integer = readScalar<uint8_t>() ? 1 : 0; return true;
                    case 'I': property.integer = readScalar<int32_t>(); return true;
                    case 'L': property.integer = readScalar<int64_t>(); return true;
                    case 'F': property.real = readScalar<float>();  property.integer = static_cast<int64_t>(property.real); return true;
                    case 'D': property.real = readScalar<double>(); property.integer = static_cast<int64_t>(property.real); return true;
                    case 'S':
                    case 'R': {
                        const uint32_t length = readScalar<uint32_t>();
                        if (m_position + length > m_size) { error = "truncated string property"; return false; }
                        property.text.assign(reinterpret_cast<const char*>(m_data + m_position), length);
                        m_position += length;
                        return true;
                    }
                    case 'f': case 'd': case 'i': case 'l': case 'b':
                        return readArray(property, type, error);
                    default:
                        error = std::string("unknown property type '") + type + "'";
                        return false;
                }
            }

            const uint8_t* m_data = nullptr;
            size_t m_size = 0;
            size_t m_position = 0;
            uint32_t m_version = 0;
            bool m_wideOffsets = false;
        };

        // ─────────────────────────────────────────────────────────────────────
        // Interpreting the tree
        // ─────────────────────────────────────────────────────────────────────

        /**
         * @brief Reads one value out of an FBX property table.
         *
         * Properties70 holds a flat list of `P` records, each `name, type, subtype, flags,
         * value...`. It is where node transforms and material colours live, addressed by string.
         */
        const FbxNode* findProperty70(const FbxNode& node, const char* wanted)
        {
            const FbxNode* properties = node.child("Properties70");
            if (!properties) return nullptr;
            for (const FbxNode& p : properties->children) {
                if (p.name == "P" && p.stringAt(0) == wanted) return &p;
            }
            return nullptr;
        }

        /// @brief The three numbers a vector-valued Properties70 entry carries, or a default.
        void readVectorProperty(const FbxNode& node, const char* wanted,
                                double fallback, double out[3])
        {
            out[0] = out[1] = out[2] = fallback;
            const FbxNode* p = findProperty70(node, wanted);
            if (!p) return;
            // name, type, subtype, flags, then x, y, z.
            for (int axis = 0; axis < 3; ++axis) {
                const size_t index = 4 + static_cast<size_t>(axis);
                if (index < p->properties.size()) out[axis] = p->properties[index].real;
            }
        }

        struct Matrix4 {
            // Column-major, matching the rest of the renderer.
            float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        };

        Matrix4 multiply(const Matrix4& a, const Matrix4& b)
        {
            Matrix4 out;
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row) {
                    float sum = 0.0f;
                    for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[column * 4 + k];
                    out.m[column * 4 + row] = sum;
                }
            }
            return out;
        }

        /**
         * @brief Builds a node's local transform from its translation, rotation and scale.
         *
         * FBX rotations are Euler angles in degrees, and the order matters: the default is XYZ,
         * meaning the X rotation is applied first. Anything with a non-default RotationOrder is
         * rare enough in exported assets that it is not handled here — a wrong order would show
         * up immediately as a prop lying on its side.
         */
        Matrix4 composeTransform(const double translation[3], const double rotation[3],
                                 const double scale[3])
        {
            const double toRadians = 3.14159265358979323846 / 180.0;
            const double sx = std::sin(rotation[0] * toRadians), cx = std::cos(rotation[0] * toRadians);
            const double sy = std::sin(rotation[1] * toRadians), cy = std::cos(rotation[1] * toRadians);
            const double sz = std::sin(rotation[2] * toRadians), cz = std::cos(rotation[2] * toRadians);

            // R = Rz * Ry * Rx
            const double r00 = cz * cy;
            const double r01 = cz * sy * sx - sz * cx;
            const double r02 = cz * sy * cx + sz * sx;
            const double r10 = sz * cy;
            const double r11 = sz * sy * sx + cz * cx;
            const double r12 = sz * sy * cx - cz * sx;
            const double r20 = -sy;
            const double r21 = cy * sx;
            const double r22 = cy * cx;

            Matrix4 out;
            out.m[0]  = static_cast<float>(r00 * scale[0]);
            out.m[1]  = static_cast<float>(r10 * scale[0]);
            out.m[2]  = static_cast<float>(r20 * scale[0]);
            out.m[3]  = 0.0f;
            out.m[4]  = static_cast<float>(r01 * scale[1]);
            out.m[5]  = static_cast<float>(r11 * scale[1]);
            out.m[6]  = static_cast<float>(r21 * scale[1]);
            out.m[7]  = 0.0f;
            out.m[8]  = static_cast<float>(r02 * scale[2]);
            out.m[9]  = static_cast<float>(r12 * scale[2]);
            out.m[10] = static_cast<float>(r22 * scale[2]);
            out.m[11] = 0.0f;
            out.m[12] = static_cast<float>(translation[0]);
            out.m[13] = static_cast<float>(translation[1]);
            out.m[14] = static_cast<float>(translation[2]);
            out.m[15] = 1.0f;
            return out;
        }

        void transformPoint(const Matrix4& t, const double in[3], float out[3])
        {
            for (int row = 0; row < 3; ++row) {
                out[row] = static_cast<float>(
                        t.m[0 * 4 + row] * in[0] + t.m[1 * 4 + row] * in[1] +
                        t.m[2 * 4 + row] * in[2] + t.m[3 * 4 + row]);
            }
        }

        /// @brief Rotates a direction. Non-uniform scale is ignored: normals are renormalised
        ///        anyway and the kits this reads use uniform scale throughout.
        void transformDirection(const Matrix4& t, const double in[3], float out[3])
        {
            for (int row = 0; row < 3; ++row) {
                out[row] = static_cast<float>(
                        t.m[0 * 4 + row] * in[0] + t.m[1 * 4 + row] * in[1] +
                        t.m[2 * 4 + row] * in[2]);
            }
            const float length = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
            if (length > 1e-8f) { out[0] /= length; out[1] /= length; out[2] /= length; }
        }

        /**
         * @brief How a layer element maps its values onto the mesh.
         *
         * The two axes are independent: mapping says *what* a value belongs to (a polygon, a
         * polygon-vertex, a control point), and reference says whether the value is stored
         * inline or behind an index. Every combination occurs in the wild.
         */
        struct LayerAccess {
            enum class Mapping { ByPolygonVertex, ByControlPoint, ByPolygon, AllSame, Unknown };
            Mapping mapping = Mapping::Unknown;
            bool indexed = false;
            const std::vector<double>* values = nullptr;
            const std::vector<int64_t>* indices = nullptr;

            /// @brief Index into @c values for a given polygon-vertex, or -1 if unavailable.
            int64_t resolve(int64_t polygonVertex, int64_t controlPoint, int64_t polygon) const
            {
                int64_t slot;
                switch (mapping) {
                    case Mapping::ByPolygonVertex: slot = polygonVertex; break;
                    case Mapping::ByControlPoint:  slot = controlPoint;  break;
                    case Mapping::ByPolygon:       slot = polygon;       break;
                    case Mapping::AllSame:         slot = 0;             break;
                    default: return -1;
                }
                if (indexed) {
                    if (!indices || slot < 0 || slot >= static_cast<int64_t>(indices->size())) return -1;
                    slot = (*indices)[static_cast<size_t>(slot)];
                }
                return slot;
            }
        };

        LayerAccess readLayerElement(const FbxNode& layer, const char* valuesName,
                                     const char* indicesName)
        {
            LayerAccess access;
            if (const FbxNode* mapping = layer.child("MappingInformationType")) {
                const std::string text = mapping->stringAt(0);
                if      (text == "ByPolygonVertex")                          access.mapping = LayerAccess::Mapping::ByPolygonVertex;
                else if (text == "ByVertex" || text == "ByControlPoint")     access.mapping = LayerAccess::Mapping::ByControlPoint;
                else if (text == "ByPolygon")                                access.mapping = LayerAccess::Mapping::ByPolygon;
                else if (text == "AllSame")                                  access.mapping = LayerAccess::Mapping::AllSame;
            }
            if (const FbxNode* reference = layer.child("ReferenceInformationType")) {
                const std::string text = reference->stringAt(0);
                access.indexed = (text == "IndexToDirect" || text == "Index");
            }
            if (const FbxNode* values = layer.child(valuesName)) {
                if (!values->properties.empty()) access.values = &values->properties[0].reals;
            }
            if (indicesName) {
                if (const FbxNode* indices = layer.child(indicesName)) {
                    if (!indices->properties.empty()) access.indices = &indices->properties[0].integers;
                }
            }
            return access;
        }

        // ─────────────────────────────────────────────────────────────────────
        // Textures by convention
        // ─────────────────────────────────────────────────────────────────────

        std::string toLower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        /**
         * @brief Finds a texture beside the model by its channel suffix.
         *
         * These kits carry no texture links in the FBX at all — the material is a bare lambert
         * named `MatID_1`, and the maps sit in the same folder as
         * `<assetId>_<resolution>_<Channel>.jpg`. Matching the suffix is therefore the only way
         * to find them.
         *
         * Two things make that less trivial than it sounds, both learned from a real pack rather
         * than from the documentation. The channel name for base colour is not one name but four
         * — `BaseColor`, `Basecolor`, `Albedo` and `Diffuse` all appear within a single download,
         * because Megascans names 3D assets and scanned surfaces differently and a kit contains
         * both. And the casing is not consistent even between those, so the comparison has to be
         * case-insensitive or it silently finds nothing and the model renders untextured white.
         *
         * @param preferredStem Model stem to prefer, e.g. `SM_His_Sal_Bar_Wood_Pack_01_A`. A kit
         *        folder can hold several meshes — modular variants A and B of the same trim — each
         *        with its own maps, so a bare suffix match would give both the same texture. A
         *        match whose name starts with this wins over one that merely ends correctly.
         * @param channels Candidate channel names, most preferred first.
         */
        std::filesystem::path findChannelTexture(const std::filesystem::path& directory,
                                                 const std::string& preferredStem,
                                                 std::initializer_list<const char*> channels)
        {
            std::error_code ec;
            if (!std::filesystem::is_directory(directory, ec)) return {};

            // Collected once, then searched per candidate, so preference order is honoured
            // regardless of the order the directory happens to enumerate in.
            std::vector<std::pair<std::string, std::filesystem::path>> candidates;
            for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                const std::filesystem::path& path = entry.path();
                const std::string extension = toLower(path.extension().string());
                if (extension != ".jpg" && extension != ".jpeg" && extension != ".png") continue;
                candidates.emplace_back(toLower(path.stem().string()), path);
            }

            const std::string wanted = toLower(preferredStem);
            for (const char* channel : channels) {
                const std::string suffix = "_" + toLower(channel);
                std::filesystem::path best;
                size_t bestLength = 0;
                bool bestIsPreferred = false;

                for (const auto& [stem, path] : candidates) {
                    if (stem.size() < suffix.size()) continue;
                    if (stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) != 0) continue;

                    const bool preferred = !wanted.empty() && stem.size() >= wanted.size()
                                        && stem.compare(0, wanted.size(), wanted) == 0;
                    // A same-stem match always beats a foreign one; among equals the shortest
                    // name wins, so `_Normal` beats a `_Normal_LOD1` variant.
                    if (best.empty()
                        || (preferred && !bestIsPreferred)
                        || (preferred == bestIsPreferred && stem.size() < bestLength)) {
                        best = path;
                        bestLength = stem.size();
                        bestIsPreferred = preferred;
                    }
                }
                if (!best.empty()) return best;
            }
            return {};
        }

    } // namespace

    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Reads a binary FBX into the renderer's mesh form.
     *
     * Handles the case these asset kits actually produce — one or more meshes, each with a single
     * material, arranged by node transforms — and normalises everything else: polygons are fanned
     * into triangles, layer elements are resolved through whichever mapping they declare, and
     * units are converted to metres so a scene assembled from several files agrees with itself.
     */
    bool loadFbx(const std::filesystem::path& path, Mesh& mesh, std::string& error)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) { error = "cannot open " + path.string(); return false; }

        const std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
            error = "cannot read " + path.string();
            return false;
        }

        FbxNode root;
        FbxReader reader(bytes.data(), bytes.size());
        if (!reader.parse(root, error)) return false;

        // ── Units and axes ──
        //
        // Megascans exports centimetres; the renderer works in metres, and a kit whose walls are
        // 225 units tall makes every distance-based default — camera speed, shadow cascades, the
        // near plane — wrong by two orders of magnitude.
        double unitScale = 1.0;
        bool zUp = false;
        if (const FbxNode* globals = root.child("GlobalSettings")) {
            if (const FbxNode* p = findProperty70(*globals, "UnitScaleFactor")) {
                if (p->properties.size() > 4) unitScale = p->properties[4].real;
            }
            if (const FbxNode* p = findProperty70(*globals, "UpAxis")) {
                if (p->properties.size() > 4) zUp = (p->properties[4].integer == 2);
            }
        }
        if (unitScale <= 0.0) unitScale = 1.0;
        const double toMetres = unitScale / 100.0;

        const FbxNode* objects = root.child("Objects");
        if (!objects) { error = "FBX has no Objects section"; return false; }

        // ── Object identities ──
        //
        // Names are stored as "Name\0\1ClassName"; only the part before the separator is the name.
        auto cleanName = [](const std::string& raw) {
            const size_t end = raw.find('\0');
            return end == std::string::npos ? raw : raw.substr(0, end);
        };

        std::unordered_set<int64_t> geometryIds;
        std::unordered_set<int64_t> modelIds;
        std::unordered_map<int64_t, std::string> modelNames;
        for (const FbxNode& node : objects->children) {
            if (node.name == "Geometry")   geometryIds.insert(node.integerAt(0));
            else if (node.name == "Model") {
                modelIds.insert(node.integerAt(0));
                modelNames[node.integerAt(0)] = cleanName(node.stringAt(1));
            }
        }

        // ── Connections: which geometry belongs to which model, and how models nest ──
        //
        // FBX stores the scene graph as a flat object list plus a separate edge list, written
        // child-first. A geometry carries no transform and a model carries no vertices, so the two
        // only become a placed mesh through the `OO` edge that joins them. Model-to-model edges
        // matter too: an exporter that emits LOD groups parents every level under a group node,
        // and a transform on that node applies to all of them.
        std::unordered_map<int64_t, int64_t> geometryToModel;
        std::unordered_map<int64_t, int64_t> modelParents;
        if (const FbxNode* connections = root.child("Connections")) {
            for (const FbxNode& c : connections->children) {
                if (c.name != "C" || c.properties.size() < 3) continue;
                if (c.stringAt(0) != "OO") continue;
                const int64_t child = c.properties[1].integer;
                const int64_t parent = c.properties[2].integer;
                if (geometryIds.count(child) && modelIds.count(parent)) {
                    geometryToModel[child] = parent;
                } else if (modelIds.count(child) && modelIds.count(parent)) {
                    modelParents[child] = parent;
                }
            }
        }

        // ── Model transforms, by object id ──
        std::unordered_map<int64_t, Matrix4> modelLocals;
        for (const FbxNode& node : objects->children) {
            if (node.name != "Model") continue;
            const int64_t id = node.integerAt(0);

            double translation[3], rotation[3], scale[3];
            readVectorProperty(node, "Lcl Translation", 0.0, translation);
            readVectorProperty(node, "Lcl Rotation",    0.0, rotation);
            readVectorProperty(node, "Lcl Scaling",     1.0, scale);
            Matrix4 local = composeTransform(translation, rotation, scale);

            // A geometric transform offsets the mesh from its node without affecting children.
            // Ignoring it silently misplaces exactly the props that use it, so it is folded in.
            double geoTranslation[3], geoRotation[3], geoScale[3];
            readVectorProperty(node, "GeometricTranslation", 0.0, geoTranslation);
            readVectorProperty(node, "GeometricRotation",    0.0, geoRotation);
            readVectorProperty(node, "GeometricScaling",     1.0, geoScale);
            const Matrix4 geometric = composeTransform(geoTranslation, geoRotation, geoScale);

            modelLocals[id] = multiply(local, geometric);
        }

        // World transform of a model, composed up its parent chain. Memoised, and depth-limited
        // against a malformed file whose parent edges form a cycle.
        std::unordered_map<int64_t, Matrix4> modelWorlds;
        std::function<Matrix4(int64_t, int)> modelWorld =
            [&](int64_t id, int depth) -> Matrix4 {
                if (const auto found = modelWorlds.find(id); found != modelWorlds.end()) {
                    return found->second;
                }
                Matrix4 local;
                if (const auto found = modelLocals.find(id); found != modelLocals.end()) {
                    local = found->second;
                }
                Matrix4 world = local;
                if (depth < 32) {
                    if (const auto parent = modelParents.find(id); parent != modelParents.end()) {
                        world = multiply(modelWorld(parent->second, depth + 1), local);
                    }
                }
                modelWorlds[id] = world;
                return world;
            };

        // ── Which of the file's meshes is the one to draw ──
        //
        // A single asset FBX from a game-ready pack is not one mesh. These carry five: LOD0
        // through LOD3 plus a `ConvexHulls` body for the physics engine. Loading all of them
        // stacks four resolutions of the same prop on top of each other and adds a set of crude
        // collision blocks that stick out through the surface — which is exactly what it looks
        // like, and it triples the triangle count while doing it.
        //
        // The level is named on the *model*, not the geometry: every geometry here is called
        // `SM_..._A Geometry` and only the model says `_LOD2`. So the filter reads the model name.
        auto isCollisionName = [](const std::string& name) {
            static const char* markers[] = { "ConvexHull", "Collision", "Collider", "UCX_", "UBX_" };
            for (const char* marker : markers) {
                if (name.find(marker) != std::string::npos) return true;
            }
            return false;
        };
        // Returns -1 when the name carries no LOD marker at all.
        auto lodLevelOf = [](const std::string& name) -> int {
            const size_t at = name.rfind("_LOD");
            if (at == std::string::npos || at + 4 >= name.size()) return -1;
            const char digit = name[at + 4];
            return (digit >= '0' && digit <= '9') ? digit - '0' : -1;
        };

        bool anyLodMarker = false;
        for (const int64_t geometryId : geometryIds) {
            const auto link = geometryToModel.find(geometryId);
            if (link == geometryToModel.end()) continue;
            if (lodLevelOf(modelNames[link->second]) >= 0) { anyLodMarker = true; break; }
        }

        // ── One material for the whole file ──
        //
        // These kits use a single material per asset and keep the maps beside the model, so the
        // material is built from the folder rather than from the FBX's own material record.
        const std::filesystem::path directory = path.parent_path();
        MeshMaterial material;
        material.name = path.stem().string();
        const std::string stem = path.stem().string();
        material.albedoTexture = findChannelTexture(directory, stem,
                                                    { "Albedo", "BaseColor", "Diffuse" });
        material.normalTexture = findChannelTexture(directory, stem, { "Normal" });
        material.alphaTexture  = findChannelTexture(directory, stem, { "Opacity", "Mask" });
        if (!material.alphaTexture.empty()) {
            // An opacity map on a kit asset means cut-out foliage, lace, or a grille — binary
            // coverage, and modelled as single planes meant to be seen from behind as well.
            material.blendMode = MaterialBlendMode::Cutout;
            material.twoSided  = true;
        }
        // Megascans surfaces are scanned dielectrics: no metal, and rougher than the 0.5 default.
        material.roughness = 0.7f;
        material.metallic  = 0.0f;
        mesh.materials.push_back(material);

        // ── Geometry ──
        size_t geometryCount = 0;
        size_t skippedCount = 0;
        for (const FbxNode& node : objects->children) {
            if (node.name != "Geometry") continue;

            // Skip collision bodies always, and every LOD but the finest when the file has them.
            // A file with no LOD naming keeps all its parts: a bottle with a separate cork is one
            // asset in two meshes, and dropping one of them is not a saving.
            {
                std::string ownerName;
                if (const auto link = geometryToModel.find(node.integerAt(0));
                    link != geometryToModel.end()) {
                    ownerName = modelNames[link->second];
                }
                if (ownerName.empty()) ownerName = cleanName(node.stringAt(1));

                if (isCollisionName(ownerName)) { ++skippedCount; continue; }
                if (anyLodMarker && lodLevelOf(ownerName) != 0) { ++skippedCount; continue; }
            }

            const FbxNode* verticesNode = node.child("Vertices");
            const FbxNode* polygonNode  = node.child("PolygonVertexIndex");
            if (!verticesNode || !polygonNode) continue;
            if (verticesNode->properties.empty() || polygonNode->properties.empty()) continue;

            const std::vector<double>& positions = verticesNode->properties[0].reals;
            const std::vector<int64_t>& polygons = polygonNode->properties[0].integers;
            if (positions.empty() || polygons.empty()) continue;

            LayerAccess normals;
            if (const FbxNode* layer = node.child("LayerElementNormal")) {
                normals = readLayerElement(*layer, "Normals", "NormalsIndex");
            }
            LayerAccess uvs;
            if (const FbxNode* layer = node.child("LayerElementUV")) {
                uvs = readLayerElement(*layer, "UV", "UVIndex");
            }

            // The transform of the model this geometry hangs off, in metres.
            Matrix4 transform;
            if (const auto link = geometryToModel.find(node.integerAt(0));
                link != geometryToModel.end()) {
                transform = modelWorld(link->second, 0);
            }
            transform.m[12] = static_cast<float>(transform.m[12] * toMetres);
            transform.m[13] = static_cast<float>(transform.m[13] * toMetres);
            transform.m[14] = static_cast<float>(transform.m[14] * toMetres);

            const uint32_t firstIndex = static_cast<uint32_t>(mesh.indices.size());

            // Polygon-vertices that share a position, a normal and a texture coordinate are the
            // same vertex; ones that differ in any of them are not, which is what makes a hard
            // edge hard. Keying on all three is the whole of the deduplication.
            struct VertexKey {
                int64_t position;
                uint32_t normal;
                float u, v;
                bool operator==(const VertexKey& other) const {
                    return position == other.position && normal == other.normal &&
                           u == other.u && v == other.v;
                }
            };
            struct VertexKeyHash {
                size_t operator()(const VertexKey& key) const {
                    size_t hash = std::hash<int64_t>()(key.position);
                    auto mix = [&hash](size_t value) {
                        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
                    };
                    mix(key.normal);
                    mix(std::hash<float>()(key.u));
                    mix(std::hash<float>()(key.v));
                    return hash;
                }
            };
            std::unordered_map<VertexKey, uint32_t, VertexKeyHash> unique;
            unique.reserve(polygons.size());

            /// Appends one polygon-vertex, reusing an identical one when it exists.
            auto emit = [&](int64_t polygonVertex, int64_t controlPoint, int64_t polygon) -> uint32_t {
                double position[3] = { 0, 0, 0 };
                const size_t base = static_cast<size_t>(controlPoint) * 3;
                if (base + 2 < positions.size()) {
                    position[0] = positions[base + 0];
                    position[1] = positions[base + 1];
                    position[2] = positions[base + 2];
                }
                if (zUp) {
                    // Z-up to Y-up, preserving handedness: (x, y, z) becomes (x, z, -y).
                    const double y = position[1];
                    position[1] = position[2];
                    position[2] = -y;
                }
                position[0] *= toMetres;
                position[1] *= toMetres;
                position[2] *= toMetres;

                double normal[3] = { 0, 1, 0 };
                if (normals.values) {
                    const int64_t slot = normals.resolve(polygonVertex, controlPoint, polygon);
                    const size_t offset = static_cast<size_t>(slot) * 3;
                    if (slot >= 0 && offset + 2 < normals.values->size()) {
                        normal[0] = (*normals.values)[offset + 0];
                        normal[1] = (*normals.values)[offset + 1];
                        normal[2] = (*normals.values)[offset + 2];
                        if (zUp) {
                            const double y = normal[1];
                            normal[1] = normal[2];
                            normal[2] = -y;
                        }
                    }
                }

                float u = 0.0f, v = 0.0f;
                if (uvs.values) {
                    const int64_t slot = uvs.resolve(polygonVertex, controlPoint, polygon);
                    const size_t offset = static_cast<size_t>(slot) * 2;
                    if (slot >= 0 && offset + 1 < uvs.values->size()) {
                        u = static_cast<float>((*uvs.values)[offset + 0]);
                        // FBX texture coordinates have V increasing upwards; the renderer samples
                        // with V down, the same flip the OBJ path applies.
                        v = 1.0f - static_cast<float>((*uvs.values)[offset + 1]);
                    }
                }

                float worldPosition[3], worldNormal[3];
                transformPoint(transform, position, worldPosition);
                transformDirection(transform, normal, worldNormal);

                const VertexKey key{ controlPoint,
                                     packNormal(worldNormal[0], worldNormal[1], worldNormal[2]),
                                     u, v };
                const auto found = unique.find(key);
                if (found != unique.end()) return found->second;

                MeshVertex vertex{};
                vertex.position[0] = worldPosition[0];
                vertex.position[1] = worldPosition[1];
                vertex.position[2] = worldPosition[2];
                vertex.packedNormal = key.normal;
                vertex.uv[0] = u;
                vertex.uv[1] = v;

                const uint32_t index = static_cast<uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back(vertex);
                unique.emplace(key, index);
                return index;
            };

            // A polygon runs until an index arrives negative; that last one is stored as ~i.
            // Polygons are fanned, which is correct for the convex quads and triangles these
            // exports contain and is what every other reader does with the general case.
            std::vector<uint32_t> corners;
            corners.reserve(4);
            int64_t polygonIndex = 0;
            for (size_t i = 0; i < polygons.size(); ++i) {
                int64_t controlPoint = polygons[i];
                const bool lastOfPolygon = controlPoint < 0;
                if (lastOfPolygon) controlPoint = ~controlPoint;

                corners.push_back(emit(static_cast<int64_t>(i), controlPoint, polygonIndex));

                if (lastOfPolygon) {
                    for (size_t corner = 2; corner < corners.size(); ++corner) {
                        mesh.indices.push_back(corners[0]);
                        mesh.indices.push_back(corners[corner - 1]);
                        mesh.indices.push_back(corners[corner]);
                    }
                    corners.clear();
                    ++polygonIndex;
                }
            }

            const uint32_t indexCount = static_cast<uint32_t>(mesh.indices.size()) - firstIndex;
            if (indexCount == 0) continue;

            MeshSubset subset;
            subset.firstIndex = firstIndex;
            subset.indexCount = indexCount;
            subset.materialIndex = 0;
            mesh.subsets.push_back(subset);
            ++geometryCount;
        }

        if (geometryCount == 0) {
            error = "FBX contains no readable mesh geometry";
            return false;
        }

        mesh.hadNormals   = true;
        mesh.hadTexCoords = true;
        mesh.sourceFormat = "FBX " + std::to_string(reader.version());
        return true;
    }

} // namespace dmrender
