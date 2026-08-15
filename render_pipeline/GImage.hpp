//
// Created by Artem Avdoshkin on 11.06.2025.
//

#ifndef RENDERING_GIMAGE_HPP
#define RENDERING_GIMAGE_HPP
#include <cstdint>
#include <string>
#include <type_traits> // For std::underlying_type

#include "Memory.hpp"

namespace dmrender {

    /**
     * @brief Maximum number of colour attachments a single render pass may write.
     *
     * Both backends guarantee at least this many (Metal's MTLRenderPassDescriptor exposes 8
     * colour attachments; Vulkan's maxColorAttachments limit is at least 4 and is 8 on every
     * desktop driver), so this is the ceiling the abstraction commits to.
     */
    inline constexpr uint32_t kMaxColorAttachments = 8;

    /**
     * @enum ImageFormat
     * @brief Defines the pixel format of an image resource.
     */
    enum class ImageFormat {
        Undefined,

        // Color formats
        RGBA8_UNORM,        ///< 8-bit per channel, unsigned normalized, RGBA order.
        BGRA8_UNORM,        ///< 8-bit per channel, unsigned normalized, BGRA order. Common for swapchains.
        RGBA16_FLOAT,       ///< 16-bit per channel, floating point.
        R32_FLOAT,          ///< Single 32-bit floating point channel.

        // Depth/stencil formats
        D32_FLOAT,          ///< 32-bit floating point depth.
        D24_UNORM_S8_UINT,  ///< 24-bit normalized depth, 8-bit unsigned integer stencil.
        D16_UNORM           ///< 16-bit normalized depth.
    };

    /**
     * @struct ClearValue
     * @brief A union-like struct specifying clear values for render pass attachments.
     */
    struct ClearValue {
        float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float depth = 1.0f;
        uint32_t stencil = 0;
    };

    /**
     * @enum ImageType
     * @brief Defines the dimensionality of an image resource.
     */
    enum class ImageType {
        Image1D,
        Image2D,
        Image3D,
        CubeMap
    };

    /**
     * @enum ImageUsage
     * @brief A bitmask specifying the intended usage of an image.
     */
    enum class ImageUsage : uint32_t {
        Sampled      = 1 << 0,  ///< Can be sampled in a shader (e.g., as a texture).
        Storage      = 1 << 1,  ///< Can be used for shader image load/store operations.
        ColorTarget  = 1 << 2,  ///< Can be used as a color render target.
        DepthStencil = 1 << 3,  ///< Can be used as a depth/stencil render target.
        TransferSrc  = 1 << 4,  ///< Can be used as a source for a copy/blit operation.
        TransferDst  = 1 << 5   ///< Can be used as a destination for a copy/blit operation.
    };

    // --- Bitmask operators for ImageUsage ---
    // This makes it a type-safe bitmask.
    inline ImageUsage operator|(ImageUsage a, ImageUsage b) {
        return static_cast<ImageUsage>(static_cast<std::underlying_type_t<ImageUsage>>(a) | static_cast<std::underlying_type_t<ImageUsage>>(b));
    }
    inline ImageUsage& operator|=(ImageUsage& a, ImageUsage b) {
        a = a | b;
        return a;
    }
    inline ImageUsage operator&(ImageUsage a, ImageUsage b) {
        return static_cast<ImageUsage>(static_cast<std::underlying_type_t<ImageUsage>>(a) & static_cast<std::underlying_type_t<ImageUsage>>(b));
    }
    inline bool hasFlag(ImageUsage flags, ImageUsage flag) {
        return (flags & flag) == flag;
    }

    /**
     * @brief Bytes occupied by a single pixel of @p format.
     * @return 0 for ImageFormat::Undefined.
     * @note Every format the abstraction exposes is uncompressed and has a whole-byte pixel
     *       size, so a row pitch is simply width * bytesPerPixel. Adding a block-compressed
     *       format later would make this function insufficient on its own.
     */
    inline uint32_t bytesPerPixel(ImageFormat format) {
        switch (format) {
            case ImageFormat::RGBA8_UNORM:
            case ImageFormat::BGRA8_UNORM:
            case ImageFormat::R32_FLOAT:
            case ImageFormat::D32_FLOAT:
            case ImageFormat::D24_UNORM_S8_UINT:
                return 4;
            case ImageFormat::RGBA16_FLOAT:
                return 8;
            case ImageFormat::D16_UNORM:
                return 2;
            case ImageFormat::Undefined:
            default:
                return 0;
        }
    }

    /// @brief Passed as ImageDesc::mipLevels to request a full mip chain down to 1x1.
    inline constexpr uint32_t kFullMipChain = 0;

    /**
     * @enum SampleCount
     * @brief How many samples per pixel an image stores, for multisample anti-aliasing.
     *
     * A multisampled image cannot be sampled by a shader and cannot have mip levels. It is
     * written by a render pass and then *resolved* — averaged down to one sample per pixel —
     * into an ordinary image, which is what later passes read. See
     * RenderPassDescriptor::setResolveAttachment().
     *
     * Support beyond four samples varies; ask Device::maxSupportedSampleCount() rather than
     * assuming.
     */
    enum class SampleCount : uint32_t {
        One = 1,
        Two = 2,
        Four = 4,
        Eight = 8,
        Sixteen = 16
    };

    /**
     * @struct ImageDesc
     * @brief Everything needed to create a GImage.
     *
     * A struct rather than a parameter list because image creation has genuinely many knobs and
     * most of them have sensible defaults; naming the ones that matter at each call site reads
     * better than a row of positional arguments.
     */
    struct ImageDesc {
        ImageType type = ImageType::Image2D;
        ImageFormat format = ImageFormat::Undefined;
        uint32_t width = 1;
        uint32_t height = 1;

        /**
         * @brief Number of mip levels, or kFullMipChain for the complete chain down to 1x1.
         *
         * Levels beyond the first are generated on the GPU from level 0 when initial pixel data
         * is supplied. Without initial data they are allocated but left undefined.
         */
        uint32_t mipLevels = 1;

        /**
         * @brief Samples per pixel. Anything above One makes this a multisample image.
         *
         * A multisample image must have exactly one mip level and must not be Sampled; it exists
         * to be rendered into and then resolved.
         */
        SampleCount sampleCount = SampleCount::One;

        ImageUsage usage = ImageUsage::Sampled;
        std::string debugName;
    };

    /**
     * @class GImage
     * @brief An abstract interface for a GPU image or texture resource.
     *
     * GImage represents a multidimensional array of pixels on the GPU. It can be a texture
     * for sampling, a render target, or a storage image.
     */
    class GImage {
    public:
        virtual ~GImage() = default;

        // Prohibit copy and move operations. Images are unique resources.
        GImage(const GImage&) = delete;
        GImage& operator=(const GImage&) = delete;
        GImage(GImage&&) = delete;
        GImage& operator=(GImage&&) = delete;

        virtual uint32_t width() const = 0;
        virtual uint32_t height() const = 0;
        virtual uint32_t depth() const = 0;
        virtual uint32_t mipLevels() const = 0;
        virtual ImageFormat format() const = 0;
        virtual ImageType type() const = 0;
        virtual ImageUsage usage() const = 0;

        /// @brief Samples per pixel; SampleCount::One for an ordinary image.
        virtual SampleCount sampleCount() const = 0;

        /**
         * @brief Replaces the contents of one mip level from CPU memory.
         *
         * The data must be tightly packed: `width * bytesPerPixel(format())` bytes per row, with
         * dimensions halved (rounding down, minimum 1) for each level below zero.
         *
         * @param data Tightly packed pixels for the whole level.
         * @param dataSize Size of @p data in bytes; must match the level exactly.
         * @param mipLevel Which level to replace.
         *
         * @note Uploading level 0 does *not* refresh the other levels. Call
         *       generateMipmaps() afterwards if the image has a chain.
         * @note Synchronous: the upload is submitted and waited on before returning.
         */
        virtual void update(const void* data, size_t dataSize, uint32_t mipLevel = 0) = 0;

        /**
         * @brief Fills levels 1..mipLevels()-1 by successively downsampling level 0 on the GPU.
         *
         * Does nothing on an image with a single level. Requires the format to support linear
         * filtering, which every format this abstraction exposes does on both backends.
         */
        virtual void generateMipmaps() = 0;

        /**
         * @brief Copies one mip level back into CPU memory.
         *
         * The counterpart of update(), and the basis for screenshots, GPU picking, and comparing
         * rendered output against a reference in a test.
         *
         * @param destination Buffer to fill; receives tightly packed pixels.
         * @param destinationSize Size of @p destination in bytes; must match the level exactly.
         * @param mipLevel Which level to read.
         *
         * @pre The image must have been created with ImageUsage::TransferSrc.
         *
         * @note Synchronous and slow by nature: it waits for the GPU to finish, copies through a
         *       staging buffer, and waits again. It is a debugging and tooling facility, not
         *       something to do per frame.
         * @note A multisample image cannot be read back. Resolve it first and read the resolve
         *       target.
         */
        virtual void readback(void* destination, size_t destinationSize, uint32_t mipLevel = 0) = 0;

        /**
         * @brief Reports where this image's memory actually landed.
         * @return Always MemoryLocation::DeviceLocal for images this library creates — a render
         *         target or sampled texture is never worth placing in host-visible memory.
         *         Swapchain images report DeviceLocal as well; their memory belongs to the
         *         presentation engine and does not count against the application's budget.
         */
        virtual MemoryLocation memoryLocation() const = 0;

        /**
         * @brief Retrieves the native handle for the underlying texture resource.
         * @return A void pointer to the native object (e.g., id<MTLTexture>, VkImage).
         */
        virtual void* nativeHandle() const = 0;

        /**
         * @brief Retrieves the native handle for a drawable object, if applicable.
         * @return A void pointer to a native drawable (e.g., id<CAMetalDrawable>).
         * @note This is primarily for images obtained from a swapchain. For other images,
         * this may return nullptr.
         */
        virtual void* nativeDrawableHandle() const = 0;

        virtual const std::string& debugName() const = 0;
        virtual void setDebugName(const std::string& name) = 0;

    protected:
        GImage() = default;
    };

} // namespace dmrender
#endif //RENDERING_GIMAGE_HPP
