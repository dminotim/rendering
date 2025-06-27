#import "MetalRenderPassDescriptor.hpp"
#import "MetalImage.hpp"
#import <Metal/Metal.h>

namespace dmrender {

    struct MetalRenderPassDescriptorNativeData
    {
        MTLRenderPassDescriptor* m_pass;
    };
    MetalRenderPassDescriptor::MetalRenderPassDescriptor()
    :m_data(std::make_unique<MetalRenderPassDescriptorNativeData>()){
        m_data->m_pass = [[MTLRenderPassDescriptor renderPassDescriptor] retain];
    }

    MetalRenderPassDescriptor::~MetalRenderPassDescriptor() {
        if (m_data->m_pass) {
            [m_data->m_pass release];
            m_data->m_pass = nullptr;
        }
    }

    void MetalRenderPassDescriptor::setColorAttachment(uint32_t index,
                                                       const std::shared_ptr<GImage>& image,
                                                       bool clear,
                                                       const ClearValue& clearValue) {
        auto* attachment = m_data->m_pass.colorAttachments[index];

        auto* metalImage = static_cast<MetalImage*>(image.get());
        id<MTLTexture> tex = (__bridge id<MTLTexture>)metalImage->nativeHandle();

        attachment.texture = tex;
        attachment.loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
        attachment.storeAction = MTLStoreActionStore;
        attachment.clearColor = MTLClearColorMake(clearValue.color[0], clearValue.color[1], clearValue.color[2], clearValue.color[3]);
    }

    void MetalRenderPassDescriptor::setDepthStencilAttachment(const std::shared_ptr<GImage>& image,
                                                              bool clearDepth,
                                                              float depthValue,
                                                              bool clearStencil,
                                                              uint32_t stencilValue) {

        auto* metalImage = static_cast<MetalImage*>(image.get());
        id<MTLTexture> tex = (__bridge id<MTLTexture>)metalImage->nativeHandle();

        auto* depth = m_data->m_pass.depthAttachment;
        depth.texture = tex;
        depth.loadAction = clearDepth ? MTLLoadActionClear : MTLLoadActionLoad;
        depth.storeAction = MTLStoreActionStore;
        depth.clearDepth = depthValue;

        if (clearStencil) {
            auto* stencil = m_data->m_pass.stencilAttachment;
            stencil.texture = tex;
            stencil.loadAction = MTLLoadActionClear;
            stencil.storeAction = MTLStoreActionStore;
            stencil.clearStencil = stencilValue;
        }
    }

    void* MetalRenderPassDescriptor::nativeHandle() {
        return (__bridge void*)m_data->m_pass;
    }

}