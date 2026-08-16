#import "MetalRenderPassDescriptor.hpp"
#import "MetalImage.hpp"
#import <Metal/Metal.h>

namespace dmrender {

    struct MetalRenderPassDescriptorNativeData
    {
        MTLRenderPassDescriptor* m_pass;
        /// One past the highest index ever passed to setColorAttachment().
        uint32_t m_colorCount = 0;
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
                                                       const ClearValue& clearValue,
                                                       uint32_t arrayLayer) {
        if (index >= kMaxColorAttachments) {
            NSLog(@"[ERROR] MetalRenderPassDescriptor: colour attachment index is out of range");
            return;
        }

        auto* attachment = m_data->m_pass.colorAttachments[index];
        // `slice` picks one layer of an array or one face of a cubemap. Vulkan expresses the
        // same thing with a single-layer image view; here it is a property of the attachment.
        attachment.slice = arrayLayer;

        auto* metalImage = static_cast<MetalImage*>(image.get());
        id<MTLTexture> tex = (__bridge id<MTLTexture>)metalImage->nativeHandle();

        attachment.texture = tex;
        attachment.loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
        attachment.storeAction = MTLStoreActionStore;
        attachment.clearColor = MTLClearColorMake(clearValue.color[0], clearValue.color[1], clearValue.color[2], clearValue.color[3]);

        if (index + 1 > m_data->m_colorCount) {
            m_data->m_colorCount = index + 1;
        }
    }

    uint32_t MetalRenderPassDescriptor::colorAttachmentCount() const {
        return m_data->m_colorCount;
    }

    void MetalRenderPassDescriptor::setResolveAttachment(uint32_t index,
                                                         const std::shared_ptr<GImage>& resolveImage) {
        if (index >= kMaxColorAttachments) {
            NSLog(@"[ERROR] MetalRenderPassDescriptor: resolve attachment index is out of range");
            return;
        }
        if (!resolveImage) return;

        auto* attachment = m_data->m_pass.colorAttachments[index];
        auto* metalImage = static_cast<MetalImage*>(resolveImage.get());

        attachment.resolveTexture = (__bridge id<MTLTexture>)metalImage->nativeHandle();
        // MultisampleResolve averages the samples into resolveTexture and discards the samples
        // themselves, which is what Vulkan's DONT_CARE store op on a resolving attachment does.
        attachment.storeAction = MTLStoreActionMultisampleResolve;
    }

    void MetalRenderPassDescriptor::setDepthStencilAttachment(const std::shared_ptr<GImage>& image,
                                                              bool clearDepth,
                                                              float depthValue,
                                                              bool clearStencil,
                                                              uint32_t stencilValue,
                                                              uint32_t arrayLayer) {

        auto* metalImage = static_cast<MetalImage*>(image.get());
        id<MTLTexture> tex = (__bridge id<MTLTexture>)metalImage->nativeHandle();

        auto* depth = m_data->m_pass.depthAttachment;
        depth.texture = tex;
        depth.slice = arrayLayer;
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