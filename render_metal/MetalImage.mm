#include "MetalImage.hpp"
#import "MetalCommandQueues.hpp"
#import "MetalUtilsCpp.hpp"
#import "Device.hpp"

#include <stdexcept>

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

    MetalImage::MetalImage(Device* device,
                           ImageType type,
                           ImageFormat format,
                           uint32_t width,
                           uint32_t height,
                           ImageUsage usage,
                           const std::string& debugName)
            : m_drawable(nil),
              m_format(format),
              m_usage(usage),
              m_type(type),
              m_debugName(debugName)
    {
        if (width == 0 || height == 0) {
            throw std::runtime_error("MetalImage: zero sized image");
        }

        auto mtlDevice = (__bridge id<MTLDevice>)device->nativeHandle();

        MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
        descriptor.textureType = ToMTLTextureType(type);
        descriptor.pixelFormat = ToMTLPixelFormat(format);
        descriptor.width = width;
        descriptor.height = height;
        descriptor.depth = 1;
        descriptor.mipmapLevelCount = 1;
        descriptor.arrayLength = 1;
        descriptor.usage = ToMTLTextureUsage(usage);
        // A render target lives entirely on the GPU; Private is both the fastest option and the
        // only one that allows lossless compression for an attachment.
        descriptor.storageMode = MTLStorageModePrivate;

        // newTextureWithDescriptor: returns a +1 retained object that this class owns.
        m_texture = [mtlDevice newTextureWithDescriptor:descriptor];
        [descriptor release];

        if (!m_texture) {
            throw std::runtime_error("MetalImage: failed to create MTLTexture");
        }
        if (!debugName.empty()) {
            m_texture.label = [NSString stringWithUTF8String:debugName.c_str()];
        }
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

    ImageFormat MetalImage::format() const {
        return m_format;
    }

    ImageType MetalImage::type() const {
        return m_type;
    }

    ImageUsage MetalImage::usage() const {
        return m_usage;
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
