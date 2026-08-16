#include "TextureCache.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_WRITE
#include "stb_image.h"

namespace dmrender {

    namespace {

        struct DecodedImage {
            std::string          key;
            std::vector<uint8_t> pixels;      ///< Tightly packed RGBA8.
            uint32_t             width = 0;
            uint32_t             height = 0;
            bool                 varyingAlpha = false;
            bool                 ok = false;
            std::string          failure;
        };

        /**
         * @brief Halves an RGBA8 image by averaging 2x2 blocks, in place on @p pixels.
         *
         * Used to bring oversized source art down to the configured ceiling. A box filter is
         * crude next to a proper resampler, but the result feeds a mip chain anyway and the
         * difference is invisible; pulling in a resize library for this would not earn its place.
         */
        void halve(std::vector<uint8_t>& pixels, uint32_t& width, uint32_t& height)
        {
            const uint32_t newWidth  = std::max(1u, width / 2);
            const uint32_t newHeight = std::max(1u, height / 2);
            std::vector<uint8_t> reduced(static_cast<size_t>(newWidth) * newHeight * 4);

            for (uint32_t y = 0; y < newHeight; ++y) {
                const uint32_t y0 = std::min(y * 2, height - 1);
                const uint32_t y1 = std::min(y * 2 + 1, height - 1);
                for (uint32_t x = 0; x < newWidth; ++x) {
                    const uint32_t x0 = std::min(x * 2, width - 1);
                    const uint32_t x1 = std::min(x * 2 + 1, width - 1);
                    for (uint32_t channel = 0; channel < 4; ++channel) {
                        const uint32_t sum =
                            pixels[(static_cast<size_t>(y0) * width + x0) * 4 + channel] +
                            pixels[(static_cast<size_t>(y0) * width + x1) * 4 + channel] +
                            pixels[(static_cast<size_t>(y1) * width + x0) * 4 + channel] +
                            pixels[(static_cast<size_t>(y1) * width + x1) * 4 + channel];
                        reduced[(static_cast<size_t>(y) * newWidth + x) * 4 + channel] =
                            static_cast<uint8_t>(sum / 4);
                    }
                }
            }

            pixels.swap(reduced);
            width = newWidth;
            height = newHeight;
        }

        DecodedImage decodeOne(const std::string& path, uint32_t maxDimension)
        {
            DecodedImage result;
            result.key = path;

            int width = 0, height = 0, channelsInFile = 0;
            // Always four channels: there is no three-channel format in the abstraction, and the
            // hardware stores RGB as RGBA regardless.
            stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channelsInFile, 4);
            if (!pixels) {
                result.failure = stbi_failure_reason() ? stbi_failure_reason() : "decode failed";
                return result;
            }

            result.width  = static_cast<uint32_t>(width);
            result.height = static_cast<uint32_t>(height);
            result.pixels.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
            stbi_image_free(pixels);

            // Only worth inspecting when the file actually carried an alpha channel; stb
            // synthesises 255 otherwise, and that would read as "opaque" anyway.
            if (channelsInFile == 4 || channelsInFile == 2) {
                for (size_t i = 3; i < result.pixels.size(); i += 4) {
                    if (result.pixels[i] < 250) { result.varyingAlpha = true; break; }
                }
            }

            while ((result.width > maxDimension || result.height > maxDimension) &&
                   (result.width > 1 || result.height > 1)) {
                halve(result.pixels, result.width, result.height);
            }

            result.ok = true;
            return result;
        }

    } // namespace

    TextureCache::TextureCache(std::shared_ptr<Device> device, uint32_t maxDimension)
        : m_device(std::move(device)), m_maxDimension(std::max(1u, maxDimension))
    {
    }

    std::string TextureCache::key(const std::filesystem::path& path)
    {
        std::error_code ec;
        // Two materials may name one file differently ("textures/a.png" and "./textures/a.png").
        std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
        if (ec) canonical = path;
        std::string text = canonical.string();
        std::replace(text.begin(), text.end(), '\\', '/');
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    std::shared_ptr<GImage> TextureCache::makeSolid(const uint8_t rgba[4], const char* name)
    {
        ImageDesc desc{};
        desc.format = ImageFormat::RGBA8_UNORM;
        desc.width = 1;
        desc.height = 1;
        desc.usage = ImageUsage::Sampled;
        desc.debugName = name;
        return m_device->createImage(desc, rgba);
    }

    std::shared_ptr<GImage> TextureCache::white()
    {
        if (!m_white) {
            const uint8_t pixel[4] = { 255, 255, 255, 255 };
            m_white = makeSolid(pixel, "WhiteFallback");
        }
        return m_white;
    }

    std::shared_ptr<GImage> TextureCache::flatNormal()
    {
        if (!m_flatNormal) {
            const uint8_t pixel[4] = { 128, 128, 255, 255 };
            m_flatNormal = makeSolid(pixel, "FlatNormalFallback");
        }
        return m_flatNormal;
    }

    void TextureCache::preload(const std::vector<std::filesystem::path>& paths, bool srgb)
    {
        // Collapse to the set that still needs work, preserving the original paths so the
        // decoder can open them.
        std::vector<std::string> pending;
        std::vector<std::string> pendingKeys;
        for (const std::filesystem::path& path : paths) {
            if (path.empty()) continue;
            const std::string k = key(path);
            if (m_textures.count(k)) continue;
            if (std::find(pendingKeys.begin(), pendingKeys.end(), k) != pendingKeys.end()) continue;
            pendingKeys.push_back(k);
            pending.push_back(path.string());
        }
        if (pending.empty()) return;

        // Decode in batches rather than all at once. Three hundred 2048x2048 images decoded
        // simultaneously would be several gigabytes of staging memory held at the same time,
        // for no gain — the cores are saturated long before that.
        const unsigned threadCount = std::max(1u, std::thread::hardware_concurrency());
        const size_t batchSize = std::max<size_t>(threadCount * 2, 8);

        for (size_t start = 0; start < pending.size(); start += batchSize) {
            const size_t end = std::min(pending.size(), start + batchSize);
            std::vector<DecodedImage> decoded(end - start);

            std::atomic<size_t> next{start};
            std::vector<std::thread> workers;
            workers.reserve(threadCount);
            for (unsigned t = 0; t < threadCount; ++t) {
                workers.emplace_back([&] {
                    for (size_t i = next++; i < end; i = next++) {
                        decoded[i - start] = decodeOne(pending[i], m_maxDimension);
                    }
                });
            }
            for (std::thread& worker : workers) worker.join();

            // Upload on this thread. createImage() touches the device, the transfer command
            // buffer and the allocator; none of that is documented as thread-safe.
            for (size_t i = start; i < end; ++i) {
                DecodedImage& image = decoded[i - start];
                const std::string& k = pendingKeys[i];

                if (!image.ok) {
                    m_missing.push_back(pending[i] + " (" + image.failure + ")");
                    // A loud stand-in rather than a quiet white one: a missing albedo is a
                    // problem worth seeing, and the log line alone is easy to scroll past.
                    const uint8_t magenta[4] = { 255, 0, 255, 255 };
                    m_textures[k] = makeSolid(magenta, "MissingTexture");
                    continue;
                }

                ImageDesc desc{};
                desc.format = srgb ? ImageFormat::RGBA8_SRGB : ImageFormat::RGBA8_UNORM;
                desc.width  = image.width;
                desc.height = image.height;
                // Mips are not optional at this scale. Without them the floor moirés *and* every
                // sample misses the cache, so they cost memory and save time.
                desc.mipLevels = kFullMipChain;
                desc.usage = ImageUsage::Sampled;
                desc.debugName = std::filesystem::path(pending[i]).filename().string();

                std::shared_ptr<GImage> created = m_device->createImage(desc, image.pixels.data());
                if (!created) {
                    m_missing.push_back(pending[i] + " (createImage failed)");
                    const uint8_t magenta[4] = { 255, 0, 255, 255 };
                    created = makeSolid(magenta, "MissingTexture");
                } else {
                    // Level 0 plus the chain, which converges to 4/3 of level 0.
                    m_uploadedBytes += static_cast<uint64_t>(image.width) * image.height * 4 * 4 / 3;
                }

                m_textures[k] = created;
                m_varyingAlpha[k] = image.varyingAlpha;

                // Release the decoded pixels as we go; the batch is otherwise held to its end.
                std::vector<uint8_t>().swap(image.pixels);
            }
        }
    }

    std::shared_ptr<GImage> TextureCache::get(const std::filesystem::path& path, bool srgb)
    {
        if (path.empty()) return srgb ? white() : flatNormal();

        const std::string k = key(path);
        auto it = m_textures.find(k);
        if (it != m_textures.end()) return it->second;

        preload({ path }, srgb);
        it = m_textures.find(k);
        return it != m_textures.end() ? it->second : (srgb ? white() : flatNormal());
    }

    bool TextureCache::hasVaryingAlpha(const std::filesystem::path& path) const
    {
        if (path.empty()) return false;
        auto it = m_varyingAlpha.find(key(path));
        return it != m_varyingAlpha.end() && it->second;
    }

} // namespace dmrender
