#include "MetalCommandbuffer.hpp"
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