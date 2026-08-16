#include "Mesh.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace dmrender {

    namespace {

        /**
         * @struct SceneCacheHeader
         * @brief Identity and validity of a cached scene.
         *
         * Three fields here each guard against a distinct way this goes wrong:
         *
         * `magic` — so that a truncated download or an unrelated file is rejected rather than
         * read as garbage.
         *
         * `version` — must be bumped whenever *any* struct written below changes layout. A
         * forgotten bump means new code reading an old cache, which is silent corruption and by
         * far the most expensive mistake in this file.
         *
         * `sourceSize` and `sourceWriteTime` — so that re-exporting the model invalidates the
         * cache. Both are checked rather than just the timestamp, because version control and
         * archive extraction both restore modification times.
         */
        struct SceneCacheHeader {
            char     magic[8] = { 'D','M','S','C','N','0','0','\0' };
            uint32_t version = 2;
            uint32_t vertexStride = static_cast<uint32_t>(sizeof(MeshVertex));

            uint64_t sourceSize = 0;
            int64_t  sourceWriteTime = 0;

            uint64_t vertexCount = 0;
            uint64_t indexCount = 0;
            uint64_t subsetCount = 0;
            uint64_t materialCount = 0;

            float boundsMin[3] = { 0.0f, 0.0f, 0.0f };
            float boundsMax[3] = { 0.0f, 0.0f, 0.0f };

            uint32_t hadNormals = 0;
            uint32_t hadTexCoords = 0;
        };

        void writeString(std::ofstream& out, const std::string& value)
        {
            const uint32_t length = static_cast<uint32_t>(value.size());
            out.write(reinterpret_cast<const char*>(&length), sizeof(length));
            if (length) out.write(value.data(), length);
        }

        bool readString(std::ifstream& in, std::string& value)
        {
            uint32_t length = 0;
            in.read(reinterpret_cast<char*>(&length), sizeof(length));
            if (!in) return false;
            if (length > (1u << 20)) return false;   // implausible: treat as corruption
            value.resize(length);
            if (length) in.read(value.data(), length);
            return static_cast<bool>(in);
        }

        bool sourceStamp(const std::filesystem::path& modelPath,
                         uint64_t& outSize, int64_t& outWriteTime)
        {
            std::error_code ec;
            outSize = static_cast<uint64_t>(std::filesystem::file_size(modelPath, ec));
            if (ec) return false;
            const auto time = std::filesystem::last_write_time(modelPath, ec);
            if (ec) return false;
            outWriteTime = static_cast<int64_t>(time.time_since_epoch().count());
            return true;
        }

    } // namespace

    std::filesystem::path sceneCachePath(const std::filesystem::path& modelPath)
    {
        std::filesystem::path cache = modelPath;
        cache += ".dmcache";
        return cache;
    }

    bool loadSceneCache(const std::filesystem::path& modelPath, Mesh& mesh)
    {
        const std::filesystem::path cachePath = sceneCachePath(modelPath);
        std::ifstream in(cachePath, std::ios::binary);
        if (!in) return false;

        SceneCacheHeader header{};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!in) return false;

        const SceneCacheHeader reference{};
        if (std::memcmp(header.magic, reference.magic, sizeof(header.magic)) != 0) return false;
        if (header.version != reference.version) return false;
        if (header.vertexStride != reference.vertexStride) return false;

        uint64_t size = 0;
        int64_t writeTime = 0;
        if (!sourceStamp(modelPath, size, writeTime)) return false;
        if (header.sourceSize != size || header.sourceWriteTime != writeTime) return false;

        // Guard against a header that survived the checks but describes something impossible,
        // which would otherwise turn into a multi-gigabyte allocation.
        if (header.vertexCount > (1ull << 32) || header.indexCount > (1ull << 33)) return false;

        mesh = Mesh{};
        mesh.baseDirectory = modelPath.parent_path();
        mesh.hadNormals = header.hadNormals != 0;
        mesh.hadTexCoords = header.hadTexCoords != 0;
        std::memcpy(mesh.boundsMin, header.boundsMin, sizeof(mesh.boundsMin));
        std::memcpy(mesh.boundsMax, header.boundsMax, sizeof(mesh.boundsMax));

        mesh.vertices.resize(static_cast<size_t>(header.vertexCount));
        mesh.indices.resize(static_cast<size_t>(header.indexCount));
        mesh.subsets.resize(static_cast<size_t>(header.subsetCount));

        // The whole point of the cache: three large reads instead of parsing a gigabyte of text.
        if (!mesh.vertices.empty()) {
            in.read(reinterpret_cast<char*>(mesh.vertices.data()),
                    static_cast<std::streamsize>(mesh.vertices.size() * sizeof(MeshVertex)));
        }
        if (!mesh.indices.empty()) {
            in.read(reinterpret_cast<char*>(mesh.indices.data()),
                    static_cast<std::streamsize>(mesh.indices.size() * sizeof(uint32_t)));
        }
        if (!mesh.subsets.empty()) {
            in.read(reinterpret_cast<char*>(mesh.subsets.data()),
                    static_cast<std::streamsize>(mesh.subsets.size() * sizeof(MeshSubset)));
        }
        if (!in) return false;

        mesh.materials.resize(static_cast<size_t>(header.materialCount));
        for (MeshMaterial& material : mesh.materials) {
            std::string albedo, alpha, normal;
            if (!readString(in, material.name)) return false;
            if (!readString(in, albedo)) return false;
            if (!readString(in, alpha)) return false;
            if (!readString(in, normal)) return false;

            // Stored relative so a moved scene folder still resolves.
            material.albedoTexture = albedo.empty() ? std::filesystem::path{} : mesh.baseDirectory / albedo;
            material.alphaTexture  = alpha.empty()  ? std::filesystem::path{} : mesh.baseDirectory / alpha;
            material.normalTexture = normal.empty() ? std::filesystem::path{} : mesh.baseDirectory / normal;

            in.read(reinterpret_cast<char*>(material.baseColor), sizeof(material.baseColor));
            in.read(reinterpret_cast<char*>(material.emissive), sizeof(material.emissive));
            in.read(reinterpret_cast<char*>(&material.opacity), sizeof(material.opacity));
            in.read(reinterpret_cast<char*>(&material.roughness), sizeof(material.roughness));
            in.read(reinterpret_cast<char*>(&material.metallic), sizeof(material.metallic));

            uint32_t blendMode = 0, twoSided = 0;
            in.read(reinterpret_cast<char*>(&blendMode), sizeof(blendMode));
            in.read(reinterpret_cast<char*>(&twoSided), sizeof(twoSided));
            if (!in) return false;
            material.blendMode = static_cast<MaterialBlendMode>(blendMode);
            material.twoSided = twoSided != 0;
        }

        std::string format;
        if (!readString(in, format)) return false;
        mesh.sourceFormat = format + " (cached)";
        return true;
    }

    bool saveSceneCache(const std::filesystem::path& modelPath, const Mesh& mesh)
    {
        uint64_t size = 0;
        int64_t writeTime = 0;
        if (!sourceStamp(modelPath, size, writeTime)) return false;

        const std::filesystem::path cachePath = sceneCachePath(modelPath);
        std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
        if (!out) return false;

        SceneCacheHeader header{};
        header.sourceSize = size;
        header.sourceWriteTime = writeTime;
        header.vertexCount = mesh.vertices.size();
        header.indexCount = mesh.indices.size();
        header.subsetCount = mesh.subsets.size();
        header.materialCount = mesh.materials.size();
        std::memcpy(header.boundsMin, mesh.boundsMin, sizeof(header.boundsMin));
        std::memcpy(header.boundsMax, mesh.boundsMax, sizeof(header.boundsMax));
        header.hadNormals = mesh.hadNormals ? 1u : 0u;
        header.hadTexCoords = mesh.hadTexCoords ? 1u : 0u;

        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        if (!mesh.vertices.empty()) {
            out.write(reinterpret_cast<const char*>(mesh.vertices.data()),
                      static_cast<std::streamsize>(mesh.vertices.size() * sizeof(MeshVertex)));
        }
        if (!mesh.indices.empty()) {
            out.write(reinterpret_cast<const char*>(mesh.indices.data()),
                      static_cast<std::streamsize>(mesh.indices.size() * sizeof(uint32_t)));
        }
        if (!mesh.subsets.empty()) {
            out.write(reinterpret_cast<const char*>(mesh.subsets.data()),
                      static_cast<std::streamsize>(mesh.subsets.size() * sizeof(MeshSubset)));
        }

        auto relative = [&](const std::filesystem::path& path) -> std::string {
            if (path.empty()) return {};
            std::error_code ec;
            const std::filesystem::path rel = std::filesystem::relative(path, mesh.baseDirectory, ec);
            return (ec || rel.empty()) ? path.string() : rel.string();
        };

        for (const MeshMaterial& material : mesh.materials) {
            writeString(out, material.name);
            writeString(out, relative(material.albedoTexture));
            writeString(out, relative(material.alphaTexture));
            writeString(out, relative(material.normalTexture));

            out.write(reinterpret_cast<const char*>(material.baseColor), sizeof(material.baseColor));
            out.write(reinterpret_cast<const char*>(material.emissive), sizeof(material.emissive));
            out.write(reinterpret_cast<const char*>(&material.opacity), sizeof(material.opacity));
            out.write(reinterpret_cast<const char*>(&material.roughness), sizeof(material.roughness));
            out.write(reinterpret_cast<const char*>(&material.metallic), sizeof(material.metallic));

            const uint32_t blendMode = static_cast<uint32_t>(material.blendMode);
            const uint32_t twoSided = material.twoSided ? 1u : 0u;
            out.write(reinterpret_cast<const char*>(&blendMode), sizeof(blendMode));
            out.write(reinterpret_cast<const char*>(&twoSided), sizeof(twoSided));
        }

        // Strip any "(cached)" suffix a round trip would otherwise accumulate.
        std::string format = mesh.sourceFormat;
        const size_t suffix = format.find(" (cached)");
        if (suffix != std::string::npos) format.erase(suffix);
        writeString(out, format);

        return out.good();
    }

} // namespace dmrender
