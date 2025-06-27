//
// Created by Artem Avdoshkin on 15.06.2025.
//

#ifndef RENDERING_METALIMAGE_HPP
#define RENDERING_METALIMAGE_HPP
#include "GImage.hpp"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

namespace dmrender {
    class MetalImage : public GImage {
    public:
        explicit MetalImage(id <CAMetalDrawable> drawable,
                            ImageFormat format,
                            ImageUsage usage,
                            ImageType type,
                            const std::string &debugName = "");

        ~MetalImage() override;

        uint32_t width() const override;
        uint32_t height() const override;

        uint32_t depth() const override;

        uint32_t mipLevels() const override;

        ImageFormat format() const override;

        ImageType type() const override;


        ImageUsage usage() const override;

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
        std::string m_debugName;
    };
}

#endif //RENDERING_METALIMAGE_HPP
