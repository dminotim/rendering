#import <Metal/Metal.h>
#include "MetalPipeline.hpp"
#import "MetalUtilsCpp.hpp"

namespace dmrender
{
    struct MetalPipelineNativeData
    {
        id<MTLRenderPipelineState> m_pipelineState;
    };

    MetalPipeline::MetalPipeline(const std::shared_ptr<Device>& device,
                                 const std::shared_ptr<ShaderFunction>& vertexFunction,
                                 const std::shared_ptr<ShaderFunction>& fragmentFunction,
                                 const RenderTargetFormat& targetFormat)
            :m_data( std::make_unique<MetalPipelineNativeData>()), m_debugName("")
    {
        auto mtlDevice = (__bridge id<MTLDevice>) device->nativeHandle();
        auto vs = (__bridge id<MTLFunction>)vertexFunction->nativeHandle();
        auto fs = (__bridge id<MTLFunction>)fragmentFunction->nativeHandle();

        MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.vertexFunction = vs;
        desc.fragmentFunction = fs;
        desc.colorAttachments[0].pixelFormat = ToMTLPixelFormat(targetFormat.colorFormat);
        if (targetFormat.depthFormat != ImageFormat::Undefined)
            desc.depthAttachmentPixelFormat = ToMTLPixelFormat(targetFormat.depthFormat);
        if (targetFormat.stencilFormat != ImageFormat::Undefined)
            desc.stencilAttachmentPixelFormat = ToMTLPixelFormat(targetFormat.stencilFormat);

        NSError* error = nil;
        id<MTLRenderPipelineState> state = [mtlDevice newRenderPipelineStateWithDescriptor:desc error:&error];
        [desc release];

        if (!state) {
            NSLog(@"MetalPipeline Error: %@", error);
            m_data->m_pipelineState = nullptr;
        } else {
            m_data->m_pipelineState = state;
        }
    }

    MetalPipeline::~MetalPipeline() {
        if (m_data->m_pipelineState) {
            [m_data->m_pipelineState release];
            m_data->m_pipelineState = nullptr;
        }
    }

    void* MetalPipeline::nativeHandle() const {
        return (__bridge void*)m_data->m_pipelineState;
    }
    const std::string& MetalPipeline::debugName() const {
        return m_debugName;
    }
}