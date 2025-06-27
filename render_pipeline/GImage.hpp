//
// Created by Artem Avdoshkin on 11.06.2025.
//

#ifndef RENDERING_GIMAGE_HPP
#define RENDERING_GIMAGE_HPP
#include <cstdint>
#include <string>
#include <type_traits> // For std::underlying_type

namespace dmrender {

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
