#include "MetalImage.hpp"
#import "MetalCommandQueues.hpp"
#import "MetalDevice.hpp"
#import "MetalUtilsCpp.hpp"
#import "Device.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace dmrender {

    MetalImage::MetalImage(id<CAMetalDrawable> drawable,
                           ImageFormat format,
                           ImageUsage usage,
                           ImageType type,
                           const std::string &debugName
    )
            : m_drawable([drawable retain]),
            m_texture([
                                drawable.texture retain
                        ]),

              m_format (format),
              m_usage(usage),
              m_type(type),
              m_debugName(debugName)
    {
    }

    namespace {
        /// @brief Turns a requested level count into a concrete one, expanding kFullMipChain.
        uint32_t resolveMipLevels(uint32_t requested, uint32_t width, uint32_t height) {
            const uint32_t largest = std::max(width, height);
            uint32_t maximum = 1;
            while ((largest >> (maximum - 1)) > 1) ++maximum;

            if (requested == kFullMipChain) return maximum;
            return std::min(requested, maximum);
        }
    }

    MetalImage::MetalImage(Device* device, const ImageDesc& desc, const void* initialData)
            : m_drawable(nil),
              m_format(desc.format),
              m_usage(desc.usage),
              m_type(desc.type),
              m_device(device),
              m_debugName(desc.debugName)
    {
        if (desc.width == 0 || desc.height == 0) {
            throw std::runtime_error("MetalImage: zero sized image");
        }

        m_mipLevels = resolveMipLevels(desc.mipLevels, desc.width, desc.height);
        m_sampleCount = desc.sampleCount;

        const bool multisampled = desc.sampleCount != SampleCount::One;
        if (multisampled) {
            // Both restrictions come from Metal rather than from this wrapper; saying so here
            // beats a confusing driver failure later.
            if (m_mipLevels != 1) {
                throw std::runtime_error("MetalImage: a multisample image cannot have mip levels");
            }
            if (hasFlag(desc.usage, ImageUsage::Sampled)) {
                throw std::runtime_error(
                    "MetalImage: a multisample image cannot be sampled; render into it and "
                    "resolve into a single-sample image instead");
            }
        }

        auto mtlDevice = (__bridge id<MTLDevice>)device->nativeHandle();

        MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
        // A multisample texture is its own texture type in Metal, not a flag on the 2D one.
        descriptor.textureType = multisampled ? MTLTextureType2DMultisample
                                              : ToMTLTextureType(desc.type);
        descriptor.pixelFormat = ToMTLPixelFormat(desc.format);
        descriptor.width = desc.width;
        descriptor.height = desc.height;
        descriptor.depth = 1;
        descriptor.mipmapLevelCount = m_mipLevels;
        descriptor.sampleCount = static_cast<NSUInteger>(desc.sampleCount);
        descriptor.arrayLength = 1;
        descriptor.usage = ToMTLTextureUsage(desc.usage);
        // A render target lives entirely on the GPU; Private is both the fastest option and the
        // only one that allows lossless compression for an attachment. CPU pixels therefore
        // arrive through a staging buffer and a blit, exactly as they do on Vulkan.
        descriptor.storageMode = MTLStorageModePrivate;

        // newTextureWithDescriptor: returns a +1 retained object that this class owns.
        m_texture = [mtlDevice newTextureWithDescriptor:descriptor];
        [descriptor release];

        if (!m_texture) {
            throw std::runtime_error("MetalImage: failed to create MTLTexture");
        }
        if (!m_debugName.empty()) {
            m_texture.label = [NSString stringWithUTF8String:m_debugName.c_str()];
        }

        if (initialData) {
            update(initialData,
                   static_cast<size_t>(desc.width) * desc.height * bytesPerPixel(desc.format),
                   0);
            generateMipmaps();
        }
    }

    void MetalImage::update(const void* data, size_t dataSize, uint32_t mipLevel) {
        if (!data || dataSize == 0) return;
        if (!m_device) {
            throw std::runtime_error("GImage::update: a drawable's texture cannot be written from the CPU");
        }
        if (mipLevel >= m_mipLevels) {
            throw std::runtime_error("GImage::update: mip level is out of range");
        }

        const uint32_t levelWidth = std::max(1u, static_cast<uint32_t>(m_texture.width) >> mipLevel);
        const uint32_t levelHeight = std::max(1u, static_cast<uint32_t>(m_texture.height) >> mipLevel);
        const size_t expected = static_cast<size_t>(levelWidth) * levelHeight * bytesPerPixel(m_format);
        if (dataSize != expected) {
            throw std::runtime_error(
                "GImage::update: expected " + std::to_string(expected) + " bytes for mip level " +
                std::to_string(mipLevel) + " but received " + std::to_string(dataSize));
        }

        auto* metalDevice = static_cast<MetalDevice*>(m_device);
        metalDevice->uploadToPrivateTexture((__bridge void*)m_texture,
                                            levelWidth, levelHeight, mipLevel,
                                            data, dataSize);
    }

    void MetalImage::readback(void* destination, size_t destinationSize, uint32_t mipLevel) {
        if (!destination || destinationSize == 0) return;
        if (!m_device) {
            throw std::runtime_error("GImage::readback: a drawable's texture cannot be read back");
        }
        if (m_sampleCount != SampleCount::One) {
            throw std::runtime_error(
                "GImage::readback: a multisample image cannot be read back; resolve it first");
        }
        if (!hasFlag(m_usage, ImageUsage::TransferSrc)) {
            throw std::runtime_error(
                "GImage::readback: image was not created with ImageUsage::TransferSrc");
        }
        if (mipLevel >= m_mipLevels) {
            throw std::runtime_error("GImage::readback: mip level is out of range");
        }

        const uint32_t levelWidth = std::max(1u, static_cast<uint32_t>(m_texture.width) >> mipLevel);
        const uint32_t levelHeight = std::max(1u, static_cast<uint32_t>(m_texture.height) >> mipLevel);
        const size_t expected = static_cast<size_t>(levelWidth) * levelHeight * bytesPerPixel(m_format);
        if (destinationSize != expected) {
            throw std::runtime_error(
                "GImage::readback: expected " + std::to_string(expected) + " bytes for mip level " +
                std::to_string(mipLevel) + " but the destination is " + std::to_string(destinationSize));
        }

        auto* metalDevice = static_cast<MetalDevice*>(m_device);
        metalDevice->readbackFromPrivateTexture((__bridge void*)m_texture,
                                                levelWidth, levelHeight, mipLevel,
                                                destination, destinationSize);
    }

    void MetalImage::generateMipmaps() {
        if (m_mipLevels <= 1 || !m_device) return;
        auto* metalDevice = static_cast<MetalDevice*>(m_device);
        metalDevice->generateMipmapsForTexture((__bridge void*)m_texture);
    }

    MetalImage:: ~MetalImage() {
        if (m_texture) {
            [m_texture release];
            m_texture = nil;
        }
        if (m_drawable) {
            [m_drawable release];
            m_drawable = nil;
        }
    }

    uint32_t MetalImage::width() const {
        return static_cast<uint32_t>(m_texture.width);
    }

    uint32_t MetalImage::height() const {
        return static_cast<uint32_t>(m_texture.height);
    }

    uint32_t MetalImage::depth() const {
        return static_cast<uint32_t>(m_texture.depth);
    }

    uint32_t MetalImage::mipLevels() const {
        return static_cast<uint32_t>(m_texture.mipmapLevelCount);
    }

    // NOTE: m_mipLevels mirrors the texture's own count and exists so update() can validate a
    // level index without touching the Objective-C object on every call.

    ImageFormat MetalImage::format() const {
        return m_format;
    }

    ImageType MetalImage::type() const {
        return m_type;
    }

    ImageUsage MetalImage::usage() const {
        return m_usage;
    }

    SampleCount MetalImage::sampleCount() const {
        return m_sampleCount;
    }

    MemoryLocation MetalImage::memoryLocation() const {
        // Textures this class allocates use MTLStorageModePrivate, and a drawable's texture is
        // owned by the display system — both are GPU memory.
        return MemoryLocation::DeviceLocal;
    }

    void* MetalImage::nativeHandle() const {
        return (__bridge void*)m_texture;
    }

    const std::string& MetalImage::debugName() const {
        return m_debugName;
    }

    void MetalImage::setDebugName(const std::string& name) {
        m_debugName = name;
        m_texture.label = [NSString stringWithUTF8String:name.c_str()];
    }

    void *MetalImage::nativeDrawableHandle() const {
        return (__bridge void*)m_drawable;
    }

}
