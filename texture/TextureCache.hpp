//
// Created by Artem Avdoshkin on 16.08.2025.
//

#ifndef RENDERING_TEXTURECACHE_HPP
#define RENDERING_TEXTURECACHE_HPP

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Device.hpp"
#include "GImage.hpp"

namespace dmrender {

    /**
     * @class TextureCache
     * @brief Loads image files once and hands the same GImage to everyone who asks.
     *
     * Scenes in the archive reference the same file from many materials — San Miguel has 281
     * materials over 265 distinct albedo files — so loading per material would multiply both
     * memory and load time by the sharing factor.
     *
     * Decoding and uploading are separated on purpose. Decoding a PNG is pure CPU work and scales
     * across cores; creating the GPU image touches the device, the transfer queue and the
     * allocator, none of which this library documents as thread-safe. So decode runs on a pool
     * and upload stays on the calling thread.
     */
    class TextureCache {
    public:
        TextureCache(std::shared_ptr<Device> device, uint32_t maxDimension);

        /**
         * @brief Decodes and uploads a batch of files, reporting progress.
         *
         * Doing this up front rather than lazily is what makes the threading worthwhile: one
         * batch keeps every core busy, whereas a texture fetched on first use is one decode on
         * one thread.
         *
         * @param paths Files to load. Duplicates and already-loaded entries are skipped.
         * @param srgb Whether these are colour (true) or data (false). Colour textures are
         *             created in an sRGB format so the hardware decodes on read, before
         *             filtering — which is where a shader-side pow() gets it subtly wrong.
         */
        void preload(const std::vector<std::filesystem::path>& paths, bool srgb);

        /**
         * @brief The image for @p path, loading it now if the preload missed it.
         * @return Never null: a missing file yields a stand-in.
         */
        std::shared_ptr<GImage> get(const std::filesystem::path& path, bool srgb);

        /// @brief A 1x1 white image, for materials with no texture in a given slot.
        std::shared_ptr<GImage> white();

        /**
         * @brief A 1x1 image encoding a flat tangent-space normal, for materials with no map.
         *
         * (0.5, 0.5, 1) unpacks to (0, 0, 1) — straight out of the surface.
         */
        std::shared_ptr<GImage> flatNormal();

        /**
         * @brief Whether @p path was found to contain alpha values strictly between opaque and
         *        transparent, or fully transparent texels.
         *
         * MTL has no way to say "this material is masked" other than `map_d`, which San Miguel
         * does not use — its foliage carries the mask in the albedo image's alpha channel. The
         * only reliable signal is therefore the pixel data itself, checked once while decoding.
         *
         * @return False for files that were never loaded or have no alpha variation.
         */
        bool hasVaryingAlpha(const std::filesystem::path& path) const;

        size_t   count() const { return m_textures.size(); }
        uint64_t uploadedBytes() const { return m_uploadedBytes; }
        size_t   missingCount() const { return m_missing.size(); }
        const std::vector<std::string>& missing() const { return m_missing; }

    private:
        /// @brief Canonical form of @p path, so two spellings of one file share an entry.
        static std::string key(const std::filesystem::path& path);

        std::shared_ptr<GImage> makeSolid(const uint8_t rgba[4], const char* name);

        std::shared_ptr<Device> m_device;
        uint32_t m_maxDimension;

        std::unordered_map<std::string, std::shared_ptr<GImage>> m_textures;
        std::unordered_map<std::string, bool> m_varyingAlpha;
        std::vector<std::string> m_missing;

        std::shared_ptr<GImage> m_white;
        std::shared_ptr<GImage> m_flatNormal;
        uint64_t m_uploadedBytes = 0;
    };

} // namespace dmrender

#endif //RENDERING_TEXTURECACHE_HPP
