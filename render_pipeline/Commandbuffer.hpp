//
// Created by Artem Avdoshkin on 16.06.2025.
//

#ifndef RENDERING_COMMANDBUFFER_HPP
#define RENDERING_COMMANDBUFFER_HPP
#include <memory>
#include "RenderPassDescriptor.hpp"
#include "Pipeline.hpp"
#include "GBuffer.hpp"

namespace dmrender {

    /**
     * @enum ShaderStage
     * @brief Defines the programmable stages in the graphics pipeline.
     */
    enum class ShaderStage {
        Vertex,
        Fragment,
        Compute // For future use
    };

    /**
     * @class CommandBuffer
     * @brief An abstract interface for recording rendering commands.
     *
     * A CommandBuffer represents a sequence of commands that can be executed by the GPU.
     * The typical lifecycle is:
     * 1. Create a CommandBuffer instance.
     * 2. Begin a render pass with beginRenderPass().
     * 3. Set pipeline state, bind buffers, and issue draw calls.
     * 4. End the render pass with endRenderPass().
     * 5. Record any final commands like present().
     * 6. Commit the command buffer to a command queue for execution.
     */
    class CommandBuffer {
    public:
        virtual ~CommandBuffer() = default;
        /**
         * @brief Begins a render pass.
         * @pre No other render pass is currently active on this command buffer.
         * @param pass A shared pointer to the RenderPassDescriptor that defines the attachments and their properties.
         * @note All subsequent rendering commands must be recorded between beginRenderPass() and endRenderPass().
         */
        virtual void beginRenderPass(std::shared_ptr<RenderPassDescriptor> pass) = 0;

        /**
         * @brief Sets the active graphics pipeline state for subsequent draw calls.
         * @pre A render pass must be active.
         * @param pipeline A shared pointer to the Pipeline state object.
         */
        virtual void setRenderPipeline(std::shared_ptr<Pipeline> pipeline) = 0;
        /**
         * @brief Binds a vertex buffer to a specified slot.
         * @pre A render pass must be active.
         * @param slot The binding slot (or index) for the vertex buffer in the vertex shader.
         * @param buffer A shared pointer to the vertex GBuffer.
         * @param offset An offset in bytes from the beginning of the buffer.
         */
        virtual void setVertexBuffer(uint32_t slot, const std::shared_ptr<GBuffer>& buffer, size_t offset = 0) = 0;

        /**
         * @brief Binds a resource buffer (e.g., uniform, storage) to a specified slot for a given shader stage.
         * @pre A render pass must be active.
         * @param slot The binding slot (or index) for the buffer in the shader.
         * @param stage The shader stage to which the buffer will be bound.
         * @param buffer A shared pointer to the GBuffer.
         * @param offset An offset in bytes from the beginning of the buffer.
         */
        virtual void setUniformBuffer(uint32_t slot, ShaderStage stage, const std::shared_ptr<GBuffer>& buffer, size_t offset = 0) = 0;

        /**
         * @brief Records a non-indexed drawing command.
         * @pre A render pass and a pipeline must be active.
         * @param vertexCount The number of vertices to draw.
         * @param instanceCount The number of instances to draw.
         * @param firstVertex The index of the first vertex to draw.
         * @param firstInstance The index of the first instance to draw.
         */
        virtual void draw(uint32_t vertexCount, uint32_t instanceCount = 1,
                          uint32_t firstVertex = 0, uint32_t firstInstance = 0) = 0;

        /**
         * @brief Records an indexed drawing command.
         * @pre A render pass and a pipeline must be active.
         * @param indexBuffer A shared pointer to the GBuffer used as an index buffer.
         * @param indexType The data type of the indices (e.g., UInt16, UInt32).
         * @param indexCount The number of indices to draw.
         * @param instanceCount The number of instances to draw.
         * @param indexByteOffset The offset in bytes from the start of the index buffer to the first index.
         * @param baseVertex A value added to each index before reading from the vertex buffer.
         * @param firstInstance The index of the first instance to draw.
         */
        virtual void drawIndexed(const std::shared_ptr<GBuffer>& indexBuffer,
                                 IndexType indexType,
                                 uint32_t indexCount,
                                 uint32_t instanceCount = 1,
                                 uint32_t firstIndexOffsetBytes = 0,
                                 int32_t  vertexOffset = 0,
                                 uint32_t firstInstance = 0) = 0;

        /**
        * @brief Ends the current render pass.
        * @pre A render pass must be active.
        */
        virtual void endRenderPass() = 0;

        /**
            * @brief Schedules a presentation command for a drawable surface.
            * @param image The image (from a swapchain) to present to the screen.
            * @note This command should typically be one of the last commands recorded before commit().
        */
        virtual void present(const std::shared_ptr<GImage>& image) = 0;
        /**
             * @brief Finalizes the command buffer, making it ready for submission.
             * @note After committing, the command buffer should not be modified further.
         */
        virtual void commit() = 0;
        /**
         * @brief Retrieves the native, backend-specific handle for the command buffer.
         * @return A void pointer to the native object (e.g., id<MTLCommandBuffer>, VkCommandBuffer).
         * @note Use with caution, as this breaks the abstraction.
         */
        virtual void* nativeHandle() = 0;

        /**
         * @brief Retrieves the native, backend-specific handle for the active render command encoder.
         * @return A void pointer to the native encoder object (e.g., id<MTLRenderCommandEncoder>).
         * @warning This is a leaky abstraction! The returned handle is only valid between
         *          `beginRenderPass()` and `endRenderPass()`. Using it outside of this scope
         *          will lead to undefined behavior or crashes. This method should only be
         *          used for deep integration with backend-specific libraries like ImGui.
         */
        virtual void* nativeEncoder() = 0;
    };

} // namespace dmrender
#endif //RENDERING_COMMANDBUFFER_HPP
