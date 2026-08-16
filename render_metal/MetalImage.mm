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
        uint32_t resolveMipLevels(uint32_t requested, uint32_t width, uint32_t height, uint32_t depth) {
            const uint32_t largest = std::max(std::max(width, height), depth);
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

        // A cubemap is six layers by definition; anything else takes what it was given.
        m_arrayLayers = (desc.type == ImageType::CubeMap) ? 6 : std::max(1u, desc.arrayLayers);
        m_depth = (desc.type == ImageType::Image3D) ? std::max(1u, desc.depth) : 1;

        if (desc.type == ImageType::CubeMap && desc.arrayLayers != 1 && desc.arrayLayers != 6) {
            throw std::runtime_error("MetalImage: a cubemap has exactly 6 array layers");
        }
        if (m_depth > 1 && m_arrayLayers > 1) {
            throw std::runtime_error("MetalImage: arrays of 3D images are not supported");
        }

        if (isCompressedFormat(desc.format)) {
            // Inherent to block compression, not to this wrapper: blocks cannot be written by
            // the rasteriser, resolved, or downsampled. See the Vulkan backend for the same set.
            if (hasFlag(desc.usage, ImageUsage::ColorTarget) ||
                hasFlag(desc.usage, ImageUsage::DepthStencil) ||
                hasFlag(desc.usage, ImageUsage::Storage)) {
                throw std::runtime_error(
                    "MetalImage: a block-compressed image can only be sampled, not rendered "
                    "into or written by a shader");
            }
            if (desc.sampleCount != SampleCount::One) {
                throw std::runtime_error("MetalImage: a block-compressed image cannot be multisampled");
            }
        }

        m_mipLevels = resolveMipLevels(desc.mipLevels, desc.width, desc.height, m_depth);
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
        // Both multisampling and array-ness are texture *types* in Metal, not flags on the 2D one.
        descriptor.textureType = multisampled ? MTLTextureType2DMultisample
                                              : ToMTLTextureType(desc.type, m_arrayLayers);
        descriptor.pixelFormat = ToMTLPixelFormat(desc.format);
        descriptor.width = desc.width;
        descriptor.height = desc.height;
        descriptor.depth = m_depth;
        descriptor.mipmapLevelCount = m_mipLevels;
        descriptor.sampleCount = static_cast<NSUInteger>(desc.sampleCount);
        // Metal counts a cubemap's six faces through the texture type rather than arrayLength,
        // which stays 1 unless this is an explicit 2D array.
        descriptor.arrayLength = (desc.type == ImageType::CubeMap) ? 1 : m_arrayLayers;
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
            // One layer's worth for arrays and cubemaps, the whole volume for a 3D image.
            update(initialData, levelByteSize(0), 0, 0);
            // A compressed image's lower levels have to be supplied already compressed.
            if (!isCompressedFormat(m_format)) {
                generateMipmaps();
            }
        }
    }

    size_t MetalImage::levelByteSize(uint32_t mipLevel) const {
        // imageLevelBytes rounds up to whole blocks, so a compressed level smaller than 4x4
        // still costs one block — which is what the driver expects to be handed.
        return imageLevelBytes(m_format,
                               std::max(1u, static_cast<uint32_t>(m_texture.width) >> mipLevel),
                               std::max(1u, static_cast<uint32_t>(m_texture.height) >> mipLevel),
                               std::max(1u, m_depth >> mipLevel));
    }

    void MetalImage::update(const void* data, size_t dataSize, uint32_t mipLevel, uint32_t arrayLayer) {
        if (!data || dataSize == 0) return;
        if (!m_device) {
            throw std::runtime_error("GImage::update: a drawable's texture cannot be written from the CPU");
        }
        if (mipLevel >= m_mipLevels) {
            throw std::runtime_error("GImage::update: mip level is out of range");
        }
        if (arrayLayer >= m_arrayLayers) {
            throw std::runtime_error("GImage::update: array layer is out of range");
        }

        const size_t expected = levelByteSize(mipLevel);
        if (dataSize != expected) {
            throw std::runtime_error(
                "GImage::update: expected " + std::to_string(expected) + " bytes for mip level " +
                std::to_string(mipLevel) + " but received " + std::to_string(dataSize));
        }

        const uint32_t levelWidth = std::max(1u, static_cast<uint32_t>(m_texture.width) >> mipLevel);
        const uint32_t levelHeight = std::max(1u, static_cast<uint32_t>(m_texture.height) >> mipLevel);
        const uint32_t levelDepth = std::max(1u, m_depth >> mipLevel);

        auto* metalDevice = static_cast<MetalDevice*>(m_device);
        metalDevice->uploadToPrivateTexture((__bridge void*)m_texture,
                                            levelWidth, levelHeight, levelDepth,
                                            mipLevel, arrayLayer,
                                            data, dataSize,
                                            rowPitch(m_format, levelWidth),
                                            dataSize / levelDepth);
    }

    void MetalImage::readback(void* destination, size_t destinationSize,
                              uint32_t mipLevel, uint32_t arrayLayer) {
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
        if (arrayLayer >= m_arrayLayers) {
            throw std::runtime_error("GImage::readback: array layer is out of range");
        }

        const size_t expected = levelByteSize(mipLevel);
        if (destinationSize != expected) {
            throw std::runtime_error(
                "GImage::readback: expected " + std::to_string(expected) + " bytes for mip level " +
                std::to_string(mipLevel) + " but the destination is " + std::to_string(destinationSize));
        }

        const uint32_t levelWidth = std::max(1u, static_cast<uint32_t>(m_texture.width) >> mipLevel);
        const uint32_t levelHeight = std::max(1u, static_cast<uint32_t>(m_texture.height) >> mipLevel);
        const uint32_t levelDepth = std::max(1u, m_depth >> mipLevel);

        auto* metalDevice = static_cast<MetalDevice*>(m_device);
        metalDevice->readbackFromPrivateTexture((__bridge void*)m_texture,
                                                levelWidth, levelHeight, levelDepth,
                                                mipLevel, arrayLayer,
                                                destination, destinationSize,
                                                rowPitch(m_format, levelWidth),
                                                destinationSize / levelDepth);
    }

    void MetalImage::generateMipmaps() {
        if (m_mipLevels <= 1 || !m_device) return;
        if (isCompressedFormat(m_format)) {
            // generateMipmapsForTexture: cannot process compressed blocks. See the Vulkan backend.
            throw std::runtime_error(
                "GImage::generateMipmaps: a block-compressed image's mip levels must be supplied "
                "already compressed, one update() per level");
        }
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

    uint32_t MetalImage::arrayLayers() const {
        return m_arrayLayers;
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
