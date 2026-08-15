//
// Created by Artem Avdoshkin on 15.06.2025.
//

#ifndef RENDERING_METALIMAGE_HPP
#define RENDERING_METALIMAGE_HPP
#include <memory>

#include "GImage.hpp"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

namespace dmrender {
    class Device;

    /**
     * @class MetalImage
     * @brief GImage over either a swapchain drawable or a device-owned texture.
     *
     * The two constructors mirror the two cases the Vulkan backend also has to cover:
     *
     *  - **Drawable.** Wraps a CAMetalDrawable acquired from the layer, retaining both it and its
     *    texture. `nativeDrawableHandle()` returns the drawable, which is what
     *    `CommandBuffer::present()` needs.
     *
     *  - **Offscreen texture.** Allocates an MTLTexture through `Device::createImage()`, for use
     *    as a render target that a later pass samples. `nativeDrawableHandle()` returns nil —
     *    it can never be presented.
     */
    class MetalImage : public GImage {
    public:
        /// @brief Wraps a swapchain drawable and its texture.
        explicit MetalImage(id <CAMetalDrawable> drawable,
                            ImageFormat format,
                            ImageUsage usage,
                            ImageType type,
                            const std::string &debugName = "");

        /// @brief Creates and owns a texture, optionally filling it from CPU pixels.
        MetalImage(Device* device, const ImageDesc& desc, const void* initialData);

        ~MetalImage() override;

        uint32_t width() const override;
        uint32_t height() const override;

        uint32_t depth() const override;

        uint32_t mipLevels() const override;

        ImageFormat format() const override;

        ImageType type() const override;


        ImageUsage usage() const override;

        SampleCount sampleCount() const override;

        MemoryLocation memoryLocation() const override;

        void update(const void *data, size_t dataSize, uint32_t mipLevel = 0) override;

        void generateMipmaps() override;

        void readback(void *destination, size_t destinationSize, uint32_t mipLevel = 0) override;

        void *nativeHandle() const override;

        const std::string& debugName() const override;

        void setDebugName(const std::string &name) override;

        void *nativeDrawableHandle() const override;


    private:
        id <CAMetalDrawable> m_drawable = nil;
        id <MTLTexture> m_texture = nil;
        ImageFormat m_format;
        ImageUsage m_usage;
        ImageType m_type;
        uint32_t m_mipLevels = 1;
        SampleCount m_sampleCount = SampleCount::One;
        Device* m_device = nullptr;   ///< Set only for textures this object owns.
        std::string m_debugName;
    };
}

#endif //RENDERING_METALIMAGE_HPP
