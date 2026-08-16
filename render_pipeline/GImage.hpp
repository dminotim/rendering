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

        /**
         * @brief sRGB variants of the 8-bit formats.
         *
         * The hardware decodes sRGB to linear on read and encodes linear to sRGB on write, both
         * for free and — crucially — on the correct side of filtering. Doing the conversion with
         * pow() in a shader happens *after* the texture unit has already blended neighbouring
         * texels in sRGB space, which is subtly wrong on sharp colour boundaries.
         *
         * Use these for colour data: albedo textures, and the swapchain surface itself. Never for
         * data textures such as normal, roughness or mask maps, which are stored linearly.
         */
        RGBA8_SRGB,
        BGRA8_SRGB,

        // --- Narrow and packed colour formats ---
        //
        // Chiefly for render targets whose cost is measured in bytes per pixel per frame. A
        // G-buffer that stores a normal in RGB10A2 rather than RGBA16_FLOAT halves that
        // attachment's bandwidth at no visible quality cost, and a single-channel mask in R8
        // costs a quarter of what RGBA8 does.

        R8_UNORM,           ///< 1 byte. Masks, ambient occlusion, single-channel data.
        RG8_UNORM,          ///< 2 bytes. Octahedral normals, two-channel data.
        R16_FLOAT,          ///< 2 bytes. Half-precision single channel; linear depth at short range.
        RG16_FLOAT,         ///< 4 bytes. Velocity buffers, two-channel high-precision data.

        /**
         * @brief 4 bytes, 10 bits each for RGB and 2 for alpha.
         *
         * The standard normal format for a G-buffer: 10 bits per axis is enough that banding on
         * smooth shaded surfaces disappears, at half the cost of RGBA16_FLOAT. Unsigned, so a
         * signed normal must be mapped into [0, 1] and back.
         */
        RGB10A2_UNORM,

        /**
         * @brief 4 bytes, floating point with no alpha channel.
         *
         * Half the size of RGBA16_FLOAT for HDR colour, which matters for a full-resolution
         * lighting target or a bloom chain. The trade is no alpha and no negative values.
         */
        R11G11B10_FLOAT,

        // Depth/stencil formats
        D32_FLOAT,          ///< 32-bit floating point depth.
        D24_UNORM_S8_UINT,  ///< 24-bit normalized depth, 8-bit unsigned integer stencil.
        D16_UNORM,          ///< 16-bit normalized depth.

        // --- Block-compressed formats ---
        //
        // These store 4x4 texel blocks in a fixed number of bytes rather than storing each texel,
        // which is how a real texture set fits in a VRAM budget: BC1 is 8:1 against RGBA8 and
        // BC3/BC7 are 4:1, and the GPU decompresses while sampling at no cost. The trade is that
        // they are lossy, cannot be rendered into, and cannot have mip levels generated on the
        // GPU — a compressed texture arrives with its whole chain already compressed.
        //
        // The _SRGB variants are decoded from sRGB to linear on read, which is what colour
        // (albedo) textures want; data textures such as normal maps use the UNORM variants.

        BC1_RGBA_UNORM,     ///< 4 bits/texel. RGB plus 1-bit alpha. The cheapest colour format.
        BC1_RGBA_SRGB,      ///< BC1 with sRGB decode.
        BC3_UNORM,          ///< 8 bits/texel. RGB plus smoothly interpolated alpha.
        BC3_SRGB,           ///< BC3 with sRGB decode.
        BC4_UNORM,          ///< 4 bits/texel, single channel. Height and mask maps.
        BC5_UNORM,          ///< 8 bits/texel, two channels. The standard normal map format.
        BC7_UNORM,          ///< 8 bits/texel. Highest quality RGBA, slowest to encode.
        BC7_SRGB            ///< BC7 with sRGB decode.
    };

    /**
     * @struct FormatInfo
     * @brief Describes how a format lays out in memory.
     *
     * Uncompressed formats have a 1x1 block whose size is the pixel size, so one description
     * covers both cases and callers need no special path for compression.
     */
    struct FormatInfo {
        uint32_t blockWidth = 1;   ///< Texels per block horizontally.
        uint32_t blockHeight = 1;  ///< Texels per block vertically.
        uint32_t blockBytes = 0;   ///< Bytes one block occupies.
        bool compressed = false;
    };

    /// @brief Memory layout of @p format. blockBytes is 0 for ImageFormat::Undefined.
    inline FormatInfo formatInfo(ImageFormat format) {
        switch (format) {
            case ImageFormat::RGBA8_UNORM:
            case ImageFormat::BGRA8_UNORM:
            case ImageFormat::RGBA8_SRGB:
            case ImageFormat::BGRA8_SRGB:
            case ImageFormat::R32_FLOAT:
            case ImageFormat::RG16_FLOAT:
            case ImageFormat::RGB10A2_UNORM:
            case ImageFormat::R11G11B10_FLOAT:
            case ImageFormat::D32_FLOAT:
            case ImageFormat::D24_UNORM_S8_UINT:
                return { 1, 1, 4, false };
            case ImageFormat::RGBA16_FLOAT:
                return { 1, 1, 8, false };
            case ImageFormat::RG8_UNORM:
            case ImageFormat::R16_FLOAT:
            case ImageFormat::D16_UNORM:
                return { 1, 1, 2, false };
            case ImageFormat::R8_UNORM:
                return { 1, 1, 1, false };

            // 4x4 blocks at 8 bytes: 4 bits per texel.
            case ImageFormat::BC1_RGBA_UNORM:
            case ImageFormat::BC1_RGBA_SRGB:
            case ImageFormat::BC4_UNORM:
                return { 4, 4, 8, true };

            // 4x4 blocks at 16 bytes: 8 bits per texel.
            case ImageFormat::BC3_UNORM:
            case ImageFormat::BC3_SRGB:
            case ImageFormat::BC5_UNORM:
            case ImageFormat::BC7_UNORM:
            case ImageFormat::BC7_SRGB:
                return { 4, 4, 16, true };

            case ImageFormat::Undefined:
            default:
                return { 1, 1, 0, false };
        }
    }

    /// @brief True when @p format stores compressed blocks rather than individual texels.
    inline bool isCompressedFormat(ImageFormat format) {
        return formatInfo(format).compressed;
    }

    /**
     * @brief True when the hardware converts between sRGB and linear for this format.
     *
     * Worth checking before a shader applies its own conversion: doing both gives a picture that
     * is too dark, and doing neither gives one that is too washed out. Both look plausible enough
     * to survive a casual glance, which is why this is worth being explicit about.
     */
    inline bool isSrgbFormat(ImageFormat format) {
        switch (format) {
            case ImageFormat::RGBA8_SRGB:
            case ImageFormat::BGRA8_SRGB:
            case ImageFormat::BC1_RGBA_SRGB:
            case ImageFormat::BC3_SRGB:
            case ImageFormat::BC7_SRGB:
                return true;
            default:
                return false;
        }
    }

    /**
     * @brief Bytes one mip level of the given size occupies, tightly packed.
     *
     * Rounds up to whole blocks, which is why a 5x5 BC1 level costs the same as an 8x8 one.
     * This is the size update() and readback() expect, for every format.
     */
    /**
     * @brief Bytes between the starts of two consecutive rows of a tightly packed level.
     *
     * For an uncompressed format this is `width * bytesPerPixel`. For a compressed one it is a
     * row of *blocks*, so it covers four texel rows at once — which is why a naive
     * `size / height` gives four times the right answer and corrupts the upload.
     */
    inline size_t rowPitch(ImageFormat format, uint32_t width) {
        const FormatInfo info = formatInfo(format);
        const uint32_t blocksX = (width + info.blockWidth - 1) / info.blockWidth;
        return static_cast<size_t>(blocksX) * info.blockBytes;
    }

    inline size_t imageLevelBytes(ImageFormat format, uint32_t width, uint32_t height, uint32_t depth) {
        const FormatInfo info = formatInfo(format);
        const uint32_t blocksX = (width + info.blockWidth - 1) / info.blockWidth;
        const uint32_t blocksY = (height + info.blockHeight - 1) / info.blockHeight;
        return static_cast<size_t>(blocksX) * blocksY * depth * info.blockBytes;
    }

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
     * @brief Bytes occupied by a single pixel of an uncompressed @p format.
     * @return 0 for ImageFormat::Undefined and for any compressed format, which has no
     *         meaningful per-pixel size.
     * @note Prefer imageLevelBytes(), which is correct for every format. This remains only for
     *       code that already knows it is dealing with uncompressed data, such as a screenshot
     *       writer.
     */
    inline uint32_t bytesPerPixel(ImageFormat format) {
        const FormatInfo info = formatInfo(format);
        return info.compressed ? 0 : info.blockBytes;
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
         * @brief Number of depth slices. Only meaningful for ImageType::Image3D.
         *
         * A volume texture — density fields, colour lookup cubes, anything sampled with three
         * coordinates — is a single image with depth > 1, not a stack of 2D images.
         */
        uint32_t depth = 1;

        /**
         * @brief Number of array layers, for texture arrays and cubemaps.
         *
         * Layers are independent images of identical size and format sharing one binding, which
         * is how shadow cascades and texture atlases avoid a descriptor per slice.
         * ImageType::CubeMap always has exactly 6, ordered +X, -X, +Y, -Y, +Z, -Z; setting
         * anything else on a cubemap is an error.
         *
         * @note Mutually exclusive with depth > 1: no API here supports arrays of volumes.
         */
        uint32_t arrayLayers = 1;

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

        /// @brief Number of array layers; 6 for a cubemap, 1 for a plain 2D or 3D image.
        virtual uint32_t arrayLayers() const = 0;

        /**
         * @brief Replaces the contents of one mip level from CPU memory.
         *
         * The data must be tightly packed: `width * bytesPerPixel(format())` bytes per row, with
         * dimensions halved (rounding down, minimum 1) for each level below zero.
         *
         * What one call covers depends on the image:
         *  - 2D: the whole level, `w * h` pixels.
         *  - 3D: the whole volume for that level, `w * h * d` pixels, slices back to front.
         *  - Array or cubemap: one layer of that level, `w * h` pixels. Call once per layer.
         *
         * @param data Tightly packed pixels.
         * @param dataSize Size of @p data in bytes; must match the above exactly.
         * @param mipLevel Which level to replace.
         * @param arrayLayer Which layer to replace. Ignored for 2D and 3D images. For a cubemap
         *                   the faces are ordered +X, -X, +Y, -Y, +Z, -Z.
         *
         * @note Uploading level 0 does *not* refresh the other levels. Call
         *       generateMipmaps() afterwards if the image has a chain.
         * @note Synchronous: the upload is submitted and waited on before returning.
         */
        virtual void update(const void* data,
                            size_t dataSize,
                            uint32_t mipLevel = 0,
                            uint32_t arrayLayer = 0) = 0;

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
         * @param destinationSize Size of @p destination in bytes; must match the level exactly,
         *                        following the same rules as update().
         * @param mipLevel Which level to read.
         * @param arrayLayer Which layer to read. Ignored for 2D and 3D images.
         *
         * @pre The image must have been created with ImageUsage::TransferSrc.
         *
         * @note Synchronous and slow by nature: it waits for the GPU to finish, copies through a
         *       staging buffer, and waits again. It is a debugging and tooling facility, not
         *       something to do per frame.
         * @note A multisample image cannot be read back. Resolve it first and read the resolve
         *       target.
         */
        virtual void readback(void* destination,
                              size_t destinationSize,
                              uint32_t mipLevel = 0,
                              uint32_t arrayLayer = 0) = 0;

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
