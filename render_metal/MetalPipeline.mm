#import <Metal/Metal.h>
#include "MetalPipeline.hpp"
#import "MetalUtilsCpp.hpp"

namespace dmrender
{
    struct MetalPipelineNativeData
    {
        id<MTLRenderPipelineState> m_pipelineState = nil;
        /// Depth and stencil are a separate object in Metal; nil when neither test is enabled.
        id<MTLDepthStencilState> m_depthStencilState = nil;

        // Encoder-side state, applied when the pipeline is bound.
        MTLCullMode m_cullMode = MTLCullModeNone;
        MTLWinding m_winding = MTLWindingCounterClockwise;
        MTLTriangleFillMode m_fillMode = MTLTriangleFillModeFill;
        float m_depthBiasConstant = 0.0f;
        float m_depthBiasSlope = 0.0f;
        bool m_stencilTestEnabled = false;
        uint32_t m_stencilReference = 0;
    };

    MetalPipeline::MetalPipeline(const std::shared_ptr<Device>& device, const PipelineDesc& desc)
            : m_data(std::make_unique<MetalPipelineNativeData>()), m_debugName(desc.debugName)
    {
        const RenderTargetFormat& targetFormat = desc.targetFormat;

        if (!desc.vertexFunction || !desc.fragmentFunction) {
            NSLog(@"MetalPipeline Error: both a vertex and a fragment function are required");
            return;
        }
        if (targetFormat.colorFormats.empty()) {
            NSLog(@"MetalPipeline Error: RenderTargetFormat needs at least one colour format");
            return;
        }
        if (!desc.blendStates.empty() && desc.blendStates.size() != targetFormat.colorFormats.size()) {
            NSLog(@"MetalPipeline Error: blendStates must be empty or have one entry per colour format");
            return;
        }

        auto mtlDevice = (__bridge id<MTLDevice>) device->nativeHandle();
        auto vs = (__bridge id<MTLFunction>) desc.vertexFunction->nativeHandle();
        auto fs = (__bridge id<MTLFunction>) desc.fragmentFunction->nativeHandle();

        // --- 1. The render pipeline state: shaders, attachment formats, blending ---
        MTLRenderPipelineDescriptor* pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
        pipelineDescriptor.vertexFunction = vs;
        pipelineDescriptor.fragmentFunction = fs;
        // Must match the sample count of the textures the pass attaches, exactly as Vulkan's
        // rasterizationSamples must match its render pass.
        pipelineDescriptor.rasterSampleCount = static_cast<NSUInteger>(targetFormat.sampleCount);

        // One entry per fragment shader output: colorAttachments[i] pairs with [[color(i)]].
        for (size_t i = 0; i < targetFormat.colorFormats.size(); ++i) {
            MTLRenderPipelineColorAttachmentDescriptor* attachment = pipelineDescriptor.colorAttachments[i];
            attachment.pixelFormat = ToMTLPixelFormat(targetFormat.colorFormats[i]);

            const BlendState blend = desc.blendStates.empty() ? BlendState{} : desc.blendStates[i];
            attachment.blendingEnabled = blend.enabled ? YES : NO;
            attachment.sourceRGBBlendFactor = ToMTLBlendFactor(blend.srcColorFactor);
            attachment.destinationRGBBlendFactor = ToMTLBlendFactor(blend.dstColorFactor);
            attachment.rgbBlendOperation = ToMTLBlendOperation(blend.colorOp);
            attachment.sourceAlphaBlendFactor = ToMTLBlendFactor(blend.srcAlphaFactor);
            attachment.destinationAlphaBlendFactor = ToMTLBlendFactor(blend.dstAlphaFactor);
            attachment.alphaBlendOperation = ToMTLBlendOperation(blend.alphaOp);
            attachment.writeMask = ToMTLColorWriteMask(blend.writeMask);
        }

        const bool hasDepth = targetFormat.depthFormat != ImageFormat::Undefined;
        if (hasDepth)
            pipelineDescriptor.depthAttachmentPixelFormat = ToMTLPixelFormat(targetFormat.depthFormat);
        if (targetFormat.stencilFormat != ImageFormat::Undefined)
            pipelineDescriptor.stencilAttachmentPixelFormat = ToMTLPixelFormat(targetFormat.stencilFormat);

        if (!m_debugName.empty()) {
            pipelineDescriptor.label = [NSString stringWithUTF8String:m_debugName.c_str()];
        }

        NSError* error = nil;
        id<MTLRenderPipelineState> state =
            [mtlDevice newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
        [pipelineDescriptor release];

        if (!state) {
            NSLog(@"MetalPipeline Error: %@", error);
            m_data->m_pipelineState = nullptr;
            return;
        }
        m_data->m_pipelineState = state;

        // --- 2. The depth/stencil state, only when something is actually tested ---
        if (desc.depthStencil.depthTestEnabled || desc.depthStencil.stencilTestEnabled) {
            if (!hasDepth) {
                NSLog(@"MetalPipeline Error: depth or stencil testing was requested but "
                       "RenderTargetFormat::depthFormat is Undefined");
            } else {
                MTLDepthStencilDescriptor* depthDescriptor = [[MTLDepthStencilDescriptor alloc] init];
                // Metal has no separate "depth test enabled" flag: a compare function of Always
                // combined with writes disabled is the same as no test at all.
                depthDescriptor.depthCompareFunction = desc.depthStencil.depthTestEnabled
                    ? ToMTLCompareFunction(desc.depthStencil.depthCompareOp)
                    : MTLCompareFunctionAlways;
                depthDescriptor.depthWriteEnabled = desc.depthStencil.depthWriteEnabled ? YES : NO;

                if (desc.depthStencil.stencilTestEnabled) {
                    depthDescriptor.frontFaceStencil = ToMTLStencilDescriptor(desc.depthStencil.front);
                    depthDescriptor.backFaceStencil = ToMTLStencilDescriptor(desc.depthStencil.back);
                }

                m_data->m_depthStencilState =
                    [mtlDevice newDepthStencilStateWithDescriptor:depthDescriptor];
                [depthDescriptor release];
            }
        }

        // --- 3. Encoder-side state, remembered for setRenderPipeline() to apply ---
        m_data->m_cullMode = ToMTLCullMode(desc.rasterizer.cullMode);
        m_data->m_winding = ToMTLWinding(desc.rasterizer.frontFace);
        m_data->m_fillMode = ToMTLTriangleFillMode(desc.rasterizer.polygonMode);
        m_data->m_depthBiasConstant = desc.rasterizer.depthBiasConstant;
        m_data->m_depthBiasSlope = desc.rasterizer.depthBiasSlope;
        m_data->m_stencilTestEnabled = desc.depthStencil.stencilTestEnabled;
        // Vulkan carries a reference value per face but only one can be set on a Metal encoder;
        // the front face's value wins, which is what every renderer that uses both faces does.
        m_data->m_stencilReference = desc.depthStencil.front.reference;
    }

    MetalPipeline::~MetalPipeline() {
        if (m_data->m_depthStencilState) {
            [m_data->m_depthStencilState release];
            m_data->m_depthStencilState = nullptr;
        }
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

    void* MetalPipeline::depthStencilState() const {
        return (__bridge void*)m_data->m_depthStencilState;
    }

    void MetalPipeline::applyEncoderState(void* encoder) const {
        auto mtlEncoder = (__bridge id<MTLRenderCommandEncoder>)encoder;
        if (!mtlEncoder) return;

        [mtlEncoder setCullMode:m_data->m_cullMode];
        [mtlEncoder setFrontFacingWinding:m_data->m_winding];
        [mtlEncoder setTriangleFillMode:m_data->m_fillMode];
        [mtlEncoder setDepthBias:m_data->m_depthBiasConstant
                      slopeScale:m_data->m_depthBiasSlope
                           clamp:0.0f];

        if (m_data->m_depthStencilState) {
            [mtlEncoder setDepthStencilState:m_data->m_depthStencilState];
        }
        if (m_data->m_stencilTestEnabled) {
            [mtlEncoder setStencilReferenceValue:m_data->m_stencilReference];
        }
    }
}
