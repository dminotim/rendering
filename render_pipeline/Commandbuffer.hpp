//
// Created by Artem Avdoshkin on 16.06.2025.
//

#ifndef RENDERING_COMMANDBUFFER_HPP
#define RENDERING_COMMANDBUFFER_HPP
#include <memory>
#include "RenderPassDescriptor.hpp"
#include "Pipeline.hpp"
#include "GBuffer.hpp"
#include "GSampler.hpp"

namespace dmrender {

    /**
     * @brief Largest push constant block the abstraction guarantees.
     *
     * Vulkan only promises 128 bytes of push constant space; asking for more works on most
     * desktop hardware but is not portable, so the abstraction commits to the guaranteed minimum.
     */
    inline constexpr size_t kMaxPushConstantBytes = 128;

    /**
     * @brief Metal buffer index reserved for push constant data.
     *
     * Metal has no push constants; the equivalent is `setVertexBytes:`/`setFragmentBytes:`, which
     * still target a buffer slot. Reserving the slot just past the uniform range keeps it clear
     * of anything setUniformBuffer() might bind.
     */
    inline constexpr uint32_t kPushConstantBufferSlot = 8;

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
         * @brief Binds a buffer the shader indexes per thread.
         *
         * The counterpart of setUniformBuffer() for data too large or too varied for a uniform
         * block: instance transforms read by `[[instance_id]]`, bone matrices, a table of
         * materials. A uniform block has a portable ceiling of 16 KiB — 256 4x4 matrices — while a
         * storage buffer is limited only by memory.
         *
         * @pre A render pass must be active.
         * @pre The pipeline in use must declare this slot as BufferBindingType::Storage in
         *      PipelineDesc::bufferSlots, and the shader must declare it to match.
         *
         * @param slot The binding slot, in [0, kMaxBufferSlots).
         * @param stage The shader stage that reads it.
         * @param buffer The buffer. Any BufferType works; the type only decides which extra usage
         *               the underlying allocation carries.
         * @param offset Byte offset from the start of the buffer. Handy for feeding successive
         *               batches out of one large buffer.
         *
         * @note Slot 0 already defaults to a storage buffer, which is what setVertexBuffer() binds
         *       to. The two calls are interchangeable there; setVertexBuffer() simply reads better
         *       at the point where geometry is bound.
         */
        virtual void setStorageBuffer(uint32_t slot, ShaderStage stage, const std::shared_ptr<GBuffer>& buffer, size_t offset = 0) = 0;

        /**
         * @brief Binds a texture and its sampler to a specified slot for a given shader stage.
         * @pre A render pass must be active.
         * @pre The image must have been created with ImageUsage::Sampled.
         * @param slot The binding slot for the texture in the shader.
         * @param stage The shader stage the texture will be read from.
         * @param image The image to sample.
         * @param sampler The sampling state to read it with.
         *
         * @note Texture slots are a separate numbering space from buffer slots — texture slot 0
         *       and uniform buffer slot 0 are two different bindings. This matches Metal, where
         *       `[[texture(0)]]` and `[[buffer(0)]]` are unrelated, and is why the Vulkan backend
         *       puts textures in their own descriptor set.
         *
         * @warning Reading an image that an earlier pass in this command buffer wrote is
         *          supported and correctly synchronised. Reading one that is *also* an attachment
         *          of the currently active render pass is not.
         */
        virtual void setTexture(uint32_t slot,
                                ShaderStage stage,
                                const std::shared_ptr<GImage>& image,
                                const std::shared_ptr<GSampler>& sampler) = 0;

        /**
         * @brief Writes a small block of data straight into the command stream.
         *
         * The cheapest way to give a draw call its own parameters. Nothing is allocated, no
         * descriptor is written and no buffer is bound — the bytes travel inside the command
         * buffer itself, which makes this the right home for per-draw values like a transform,
         * a material index or a colour tint. A uniform buffer remains the right home for data
         * shared across many draws.
         *
         * @pre A render pass and a pipeline must be active.
         * @param stage Which shader stages read the data.
         * @param data The bytes to write.
         * @param size Size of @p data in bytes; must not exceed kMaxPushConstantBytes.
         * @param offset Byte offset within the push constant block.
         *
         * @note The two backends express this differently in shader source, which is the one
         *       place they diverge: GLSL declares `layout(push_constant) uniform`, while MSL
         *       receives an ordinary constant reference at buffer index kPushConstantBufferSlot.
         *       The C++ call is identical either way.
         */
        virtual void setPushConstants(ShaderStage stage,
                                      const void* data,
                                      size_t size,
                                      size_t offset = 0) = 0;

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
         * @brief Issues draws whose arguments the GPU reads from a buffer.
         *
         * The CPU never sees how many vertices or instances each draw needs — it only says
         * "execute the commands in this buffer". That is what makes the draw list buildable
         * without the CPU: today by writing it once and reusing it, later by having a compute
         * shader cull and fill it.
         *
         * @pre A render pass and a pipeline must be active.
         * @param argumentBuffer A BufferType::Indirect buffer of DrawIndirectCommand structs.
         * @param drawCount How many commands to execute.
         * @param offset Byte offset of the first command.
         * @param stride Bytes between commands; 0 means tightly packed.
         *
         * @note Whether the backend can issue all @p drawCount draws in one call depends on the
         *       hardware. When it cannot, it loops — the result is identical, only the CPU cost
         *       differs.
         */
        virtual void drawIndirect(const std::shared_ptr<GBuffer>& argumentBuffer,
                                  uint32_t drawCount,
                                  size_t offset = 0,
                                  uint32_t stride = 0) = 0;

        /**
         * @brief The indexed form of drawIndirect().
         *
         * @pre A render pass and a pipeline must be active.
         * @param indexBuffer The index buffer the commands index into.
         * @param indexType The data type of the indices.
         * @param argumentBuffer A BufferType::Indirect buffer of DrawIndexedIndirectCommand structs.
         * @param drawCount How many commands to execute.
         * @param offset Byte offset of the first command.
         * @param stride Bytes between commands; 0 means tightly packed.
         */
        virtual void drawIndexedIndirect(const std::shared_ptr<GBuffer>& indexBuffer,
                                         IndexType indexType,
                                         const std::shared_ptr<GBuffer>& argumentBuffer,
                                         uint32_t drawCount,
                                         size_t offset = 0,
                                         uint32_t stride = 0) = 0;

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
