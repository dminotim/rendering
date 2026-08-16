#include "MetalCommandbuffer.hpp"
#import "MetalPipeline.hpp"
#import "SwapChain.hpp" // Required for context, even if not directly used.
#import <QuartzCore/CAMetalLayer.h> // Required for id<CAMetalDrawable>
#import <Metal/Metal.h>
#import <cassert>

namespace dmrender {

// PIMPL data structure to hide Objective-C types from the C++ header.
    struct MetalCommandBufferData
    {
        // These are the native Metal objects managed by this class.
        // Assuming Manual Retain-Release (MRR) based on the explicit 'release' calls.
        id<MTLCommandQueue> m_queue = nil;
        id<MTLCommandBuffer> m_commandBuffer = nil;
        id<MTLRenderCommandEncoder> m_encoder = nil;
    };

// --- Constructor & Destructor ---

    MetalCommandBuffer::MetalCommandBuffer(const std::shared_ptr<CommandQueue>& cmdQueue)
            : m_data(std::make_unique<MetalCommandBufferData>())
    {
        // The command queue is not retained here, as its lifetime is managed by its C++ wrapper.
        // We are holding a non-owning reference to it.
        m_data->m_queue = (__bridge id<MTLCommandQueue>) cmdQueue->nativeHandle();

        // Create a new command buffer from the queue.
        // '[... commandBuffer]' returns an autoreleased object. To take ownership and manage its
        // lifetime manually, we must call 'retain'.
        m_data->m_commandBuffer = [[m_data->m_queue commandBuffer] retain];
    }

    MetalCommandBuffer::~MetalCommandBuffer()
    {
        // --- Cleanup must be done in the reverse order of creation ---

        // If an encoder exists, it means endRenderPass() was not called. This is an API usage error.
        // We release it here to prevent a memory leak, but this situation indicates a logic flaw in the calling code.
        if (m_data->m_encoder) {
            [m_data->m_encoder release];
            m_data->m_encoder = nil;
        }

        // Release the command buffer that this object owns.
        if (m_data->m_commandBuffer) {
            [m_data->m_commandBuffer release];
            m_data->m_commandBuffer = nil;
        }

        // The m_data (unique_ptr) is automatically destroyed at the end of this scope,
        // which calls the destructor for MetalCommandBufferData.
    }

// --- Native Handle Accessors ---

    void* MetalCommandBuffer::nativeHandle()
    {
        return (__bridge void*)m_data->m_commandBuffer;
    }

    void* MetalCommandBuffer::nativeEncoder()
    {
        return (__bridge void*)m_data->m_encoder;
    }

// --- Command Recording ---

    void MetalCommandBuffer::beginRenderPass(const std::shared_ptr<RenderPassDescriptor> pass)
    {
        // This assert helps catch API misuse during development.
        assert(m_data->m_encoder == nil && "Cannot begin a new render pass while another is active.");

        auto mtlPass = (__bridge MTLRenderPassDescriptor*)pass->nativeHandle();

        // Create a new render command encoder. The method returns an autoreleased object, so we must retain it
        // to extend its lifetime beyond the current scope.
        m_data->m_encoder = [[m_data->m_commandBuffer renderCommandEncoderWithDescriptor:mtlPass] retain];
    }

    void MetalCommandBuffer::setRenderPipeline(const std::shared_ptr<Pipeline> pipeline)
    {
        assert(m_data->m_encoder != nil && "No active render pass to set a pipeline on.");
        auto mtlPipeline = (__bridge id<MTLRenderPipelineState>)pipeline->nativeHandle();
        [m_data->m_encoder setRenderPipelineState:mtlPipeline];

        // Metal keeps depth/stencil and rasterisation outside the pipeline state object, so
        // binding a Pipeline has to apply them separately for the two backends to behave the
        // same way. See MetalPipeline's class documentation.
        auto* metalPipeline = static_cast<MetalPipeline*>(pipeline.get());
        metalPipeline->applyEncoderState((__bridge void*)m_data->m_encoder);
    }

    void MetalCommandBuffer::setVertexBuffer(uint32_t slot, const std::shared_ptr<GBuffer>& buffer, size_t offset)
    {
        assert(m_data->m_encoder != nil && "No active render pass.");
        if (!buffer) return;

        auto mtlBuf = (__bridge id<MTLBuffer>)buffer->nativeHandle();
        [m_data->m_encoder setVertexBuffer:mtlBuf offset:offset atIndex:slot];
    }

    void MetalCommandBuffer::setUniformBuffer(uint32_t slot, ShaderStage stage, const std::shared_ptr<GBuffer>& buffer, size_t offset)
    {
        assert(m_data->m_encoder != nil && "No active render pass.");
        if (!buffer) return;

        id<MTLBuffer> mtlBuf = (__bridge id<MTLBuffer>)buffer->nativeHandle();
        switch (stage) {
            case ShaderStage::Vertex:
                // For uniforms in the vertex stage, Metal uses the same binding command as for vertex buffers.
                [m_data->m_encoder setVertexBuffer:mtlBuf offset:offset atIndex:slot];
                break;
            case ShaderStage::Fragment:
                [m_data->m_encoder setFragmentBuffer:mtlBuf offset:offset atIndex:slot];
                break;
            case ShaderStage::Compute:
                // This is an API usage error. A render encoder cannot handle compute stages.
                NSLog(@"[ERROR] Trying to set a buffer for Compute stage on a RenderCommandEncoder.");
                assert(false && "Invalid shader stage for render command encoder");
                break;
        }
    }

    void MetalCommandBuffer::setStorageBuffer(uint32_t slot, ShaderStage stage, const std::shared_ptr<GBuffer>& buffer, size_t offset)
    {
        // Metal draws no distinction between a uniform block and a storage buffer: both are just
        // a buffer bound at an index, and only the shader's address space qualifier
        // (`constant T&` versus `const device T*`) differs. Vulkan has to know the difference
        // because it is baked into the descriptor set layout, which is why the two setters exist
        // at all; here they are the same call.
        setUniformBuffer(slot, stage, buffer, offset);
    }

    void MetalCommandBuffer::setTexture(uint32_t slot, ShaderStage stage, const std::shared_ptr<GImage>& image,
                                        const std::shared_ptr<GSampler>& sampler)
    {
        assert(m_data->m_encoder != nil && "No active render pass.");
        if (!image || !sampler) return;

        // Metal numbers textures independently of buffers, so `slot` here is the [[texture(n)]]
        // index and has nothing to do with the [[buffer(n)]] index of the same number. The Vulkan
        // backend reproduces that by putting textures in their own descriptor set.
        id<MTLTexture> mtlTexture = (__bridge id<MTLTexture>)image->nativeHandle();
        id<MTLSamplerState> mtlSampler = (__bridge id<MTLSamplerState>)sampler->nativeHandle();

        switch (stage) {
            case ShaderStage::Vertex:
                [m_data->m_encoder setVertexTexture:mtlTexture atIndex:slot];
                [m_data->m_encoder setVertexSamplerState:mtlSampler atIndex:slot];
                break;
            case ShaderStage::Fragment:
                [m_data->m_encoder setFragmentTexture:mtlTexture atIndex:slot];
                [m_data->m_encoder setFragmentSamplerState:mtlSampler atIndex:slot];
                break;
            case ShaderStage::Compute:
                NSLog(@"[ERROR] Trying to set a texture for Compute stage on a RenderCommandEncoder.");
                assert(false && "Invalid shader stage for render command encoder");
                break;
        }
    }

    void MetalCommandBuffer::setPushConstants(ShaderStage stage, const void* data, size_t size, size_t offset)
    {
        assert(m_data->m_encoder != nil && "No active render pass.");
        if (!data || size == 0) return;
        if (offset + size > kMaxPushConstantBytes) {
            NSLog(@"[ERROR] setPushConstants writes past the guaranteed push constant block");
            return;
        }

        // Metal has no push constants. setVertexBytes:/setFragmentBytes: is the equivalent
        // mechanism: small data copied straight into the command buffer with no backing
        // MTLBuffer, bound at a reserved slot the uniform range never uses.
        //
        // Unlike Vulkan there is no addressable block to write into at an offset, so a partial
        // write is not expressible; callers pushing a whole struct at offset zero — which is the
        // normal use — behave identically on both backends.
        if (offset != 0) {
            NSLog(@"[ERROR] setPushConstants with a non-zero offset is not supported on Metal");
            return;
        }

        switch (stage) {
            case ShaderStage::Vertex:
                [m_data->m_encoder setVertexBytes:data length:size atIndex:kPushConstantBufferSlot];
                break;
            case ShaderStage::Fragment:
                [m_data->m_encoder setFragmentBytes:data length:size atIndex:kPushConstantBufferSlot];
                break;
            case ShaderStage::Compute:
                NSLog(@"[ERROR] Trying to set push constants for Compute stage on a RenderCommandEncoder.");
                assert(false && "Invalid shader stage for render command encoder");
                break;
        }
    }

    void MetalCommandBuffer::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
    {
        assert(m_data->m_encoder != nil && "No active render pass.");
        [m_data->m_encoder drawPrimitives:MTLPrimitiveTypeTriangle
                              vertexStart:firstVertex
                              vertexCount:vertexCount
                            instanceCount:instanceCount
                             baseInstance:firstInstance];
    }

    void MetalCommandBuffer::drawIndexed(const std::shared_ptr<GBuffer>& indexBuffer, IndexType idxType, uint32_t indexCount,
                                         uint32_t instanceCount, uint32_t firstIndexOffsetBytes, int32_t vertexOffset,
                                         uint32_t firstInstance)
    {
        assert(m_data->m_encoder != nil && "No active render pass.");
        if (!indexBuffer) return;

        auto mtlIndexBuffer = (__bridge id<MTLBuffer>)indexBuffer->nativeHandle();

        // Map the engine's index type enum to Metal's native enum.
        MTLIndexType mtlIndexType;
        switch (idxType) {
            case IndexType::UInt16:
                mtlIndexType = MTLIndexTypeUInt16;
                break;
            case IndexType::UInt32:
                mtlIndexType = MTLIndexTypeUInt32;
                break;
            default:
                // This should not happen if the API is used correctly.
                NSLog(@"[ERROR] Unsupported index type for drawIndexed.");
                assert(false && "Unsupported index type");
                return;
        }

        [m_data->m_encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                      indexCount:indexCount
                                       indexType:mtlIndexType
                                     indexBuffer:mtlIndexBuffer
                               indexBufferOffset:firstIndexOffsetBytes
                                   instanceCount:instanceCount
                                      baseVertex:vertexOffset
                                    baseInstance:firstInstance];
    }


    void MetalCommandBuffer::drawIndirect(const std::shared_ptr<GBuffer>& argumentBuffer,
                                          uint32_t drawCount,
                                          size_t offset,
                                          uint32_t stride)
    {
        assert(m_data->m_encoder != nil && "No active render pass.");
        if (!argumentBuffer || drawCount == 0) return;
        if (argumentBuffer->type() != BufferType::Indirect) {
            NSLog(@"[ERROR] drawIndirect: argument buffer must be BufferType::Indirect");
            return;
        }

        auto mtlArguments = (__bridge id<MTLBuffer>)argumentBuffer->nativeHandle();
        const NSUInteger effectiveStride = (stride != 0) ? stride : sizeof(DrawIndirectCommand);

        // Metal's indirect draw executes exactly one command, so multiple draws mean multiple
        // calls. The Vulkan backend can collapse them when multiDrawIndirect is available; the
        // draws issued are identical either way.
        for (uint32_t i = 0; i < drawCount; ++i) {
            [m_data->m_encoder drawPrimitives:MTLPrimitiveTypeTriangle
                               indirectBuffer:mtlArguments
                         indirectBufferOffset:offset + i * effectiveStride];
        }
    }

    void MetalCommandBuffer::drawIndexedIndirect(const std::shared_ptr<GBuffer>& indexBuffer,
                                                 IndexType indexType,
                                                 const std::shared_ptr<GBuffer>& argumentBuffer,
                                                 uint32_t drawCount,
                                                 size_t offset,
                                                 uint32_t stride)
    {
        assert(m_data->m_encoder != nil && "No active render pass.");
        if (!indexBuffer || !argumentBuffer || drawCount == 0) return;
        if (argumentBuffer->type() != BufferType::Indirect) {
            NSLog(@"[ERROR] drawIndexedIndirect: argument buffer must be BufferType::Indirect");
            return;
        }

        auto mtlIndexBuffer = (__bridge id<MTLBuffer>)indexBuffer->nativeHandle();
        auto mtlArguments = (__bridge id<MTLBuffer>)argumentBuffer->nativeHandle();
        const MTLIndexType mtlIndexType =
            (indexType == IndexType::UInt16) ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
        const NSUInteger effectiveStride =
            (stride != 0) ? stride : sizeof(DrawIndexedIndirectCommand);

        for (uint32_t i = 0; i < drawCount; ++i) {
            [m_data->m_encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                           indexType:mtlIndexType
                                         indexBuffer:mtlIndexBuffer
                                   indexBufferOffset:0
                                      indirectBuffer:mtlArguments
                                indirectBufferOffset:offset + i * effectiveStride];
        }
    }

    void MetalCommandBuffer::endRenderPass()
    {
        assert(m_data->m_encoder != nil && "endRenderPass() called without an active render pass.");

        // Finalize the encoding process for this render pass.
        [m_data->m_encoder endEncoding];

        // Release the encoder object as it is now invalid and its lifecycle is over.
        [m_data->m_encoder release];
        m_data->m_encoder = nil;
    }

    void MetalCommandBuffer::present(const std::shared_ptr<GImage>& image)
    {
        // Schedule the presentation of the drawable after this command buffer has finished executing.
        auto d = (__bridge id<CAMetalDrawable>)image->nativeDrawableHandle(); // Restored original call
        [m_data->m_commandBuffer presentDrawable:d];
    }

    void MetalCommandBuffer::commit()
    {
        // Finalize the command buffer and submit it to the queue for execution.
        // After this call, the command buffer can no longer be modified.
        [m_data->m_commandBuffer commit];
    }

} // namespace dmrender