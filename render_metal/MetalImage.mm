#include "MetalImage.hpp"
#import "MetalCommandQueues.hpp"


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
