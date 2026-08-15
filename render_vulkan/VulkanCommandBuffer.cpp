#include "render_vulkan/VulkanCommandBuffer.hpp"

#include <array>
#include <stdexcept>
#include <vector>

#include <render_vulkan/VulkanBuffer.hpp>
#include <render_vulkan/VulkanCommandQueue.hpp>
#include <render_vulkan/VulkanDevice.hpp>
#include <render_vulkan/VulkanImage.hpp>
#include <render_vulkan/VulkanPipeline.hpp>
#include <render_vulkan/VulkanRenderPassDescriptor.hpp>
#include <render_vulkan/VulkanSwapChain.hpp>
#include <render_vulkan/VulkanUtils.hpp>

namespace dmrender
{

    /// @brief One remembered `setVertexBuffer` / `setUniformBuffer` call.
    struct BoundBuffer
    {
        VulkanBuffer* buffer = nullptr;
        VkDeviceSize offset = 0;
        VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    };

    struct VulkanCommandBufferData
    {
        std::shared_ptr<CommandQueue> queueRef;   ///< Keeps the queue alive while recording.
        VulkanCommandQueues* queue = nullptr;
        VulkanDevice* device = nullptr;
        VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;

        VulkanPipeline* pipeline = nullptr;
        std::array<BoundBuffer, kMaxBindingSlots> bindings{};

        bool renderPassActive = false;
        bool committed = false;

        /// Filled by present(): which swapchain image this frame ends up on.
        VulkanSwapChain* presentSwapChain = nullptr;
        uint32_t presentImageIndex = 0;
    };

    VulkanCommandBuffer::VulkanCommandBuffer(const std::shared_ptr<CommandQueue>& cmdQueue)
        : m_data(std::make_unique<VulkanCommandBufferData>())
    {
        if (!cmdQueue) throw std::runtime_error("VulkanCommandBuffer: null command queue");

        m_data->queueRef = cmdQueue;
        m_data->queue = static_cast<VulkanCommandQueues*>(cmdQueue.get());
        m_data->device = static_cast<VulkanDevice*>(cmdQueue->getDevice().get());
        m_data->cmdBuffer = m_data->queue->currentCommandBuffer();

        // The queue already reset this buffer in beginFrame(); open recording right away so the
        // object behaves like a freshly created MTLCommandBuffer.
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkCheck(vkBeginCommandBuffer(m_data->cmdBuffer, &beginInfo), "vkBeginCommandBuffer");
    }

    VulkanCommandBuffer::~VulkanCommandBuffer()
    {
        if (m_data->committed) return;

        // Recording was opened but never committed. Close it so the buffer is left in a state the
        // queue can reset, and report the API misuse the way MetalCommandBuffer's assert does.
        if (m_data->renderPassActive) {
            vkCmdEndRenderPass(m_data->cmdBuffer);
            m_data->renderPassActive = false;
        }
        vkEndCommandBuffer(m_data->cmdBuffer);
    }

    void VulkanCommandBuffer::beginRenderPass(std::shared_ptr<RenderPassDescriptor> pass)
    {
        if (m_data->renderPassActive) {
            throw std::runtime_error("beginRenderPass: a render pass is already active");
        }

        auto* descriptor = static_cast<VulkanRenderPassDescriptor*>(pass.get());
        const auto& color = descriptor->colorAttachment();
        if (!color.isValid()) {
            throw std::runtime_error("beginRenderPass: colour attachment 0 was never set");
        }

        auto* image = static_cast<VulkanImage*>(color.image.get());
        VulkanSwapChain* swapChain = image->swapChain();
        if (!swapChain) {
            throw std::runtime_error("beginRenderPass: attachment does not come from a swapchain");
        }

        const auto& depth = descriptor->depthStencilAttachment();

        RenderPassKey key{};
        key.colorFormat = ToVkFormat(image->format());
        key.clearColor = color.clear;
        if (depth.isValid()) {
            key.depthFormat = ToVkFormat(depth.image->format());
            key.clearDepth = depth.clear;
        }

        const VkRenderPass renderPass = m_data->device->acquireRenderPass(key);
        descriptor->setResolvedRenderPass(renderPass);
        const VkFramebuffer framebuffer = swapChain->acquireFramebuffer(image->imageIndex(), renderPass);

        std::vector<VkClearValue> clearValues;
        VkClearValue colorClear{};
        colorClear.color = { { color.clearValue.color[0], color.clearValue.color[1],
                               color.clearValue.color[2], color.clearValue.color[3] } };
        clearValues.push_back(colorClear);
        if (depth.isValid()) {
            VkClearValue depthClear{};
            depthClear.depthStencil = { depth.clearValue.depth, depth.clearValue.stencil };
            clearValues.push_back(depthClear);
        }

        const VkExtent2D extent{ image->width(), image->height() };

        VkRenderPassBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = renderPass;
        beginInfo.framebuffer = framebuffer;
        beginInfo.renderArea.offset = { 0, 0 };
        beginInfo.renderArea.extent = extent;
        beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        beginInfo.pClearValues = clearValues.data();

        vkCmdBeginRenderPass(m_data->cmdBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
        m_data->renderPassActive = true;

        // Metal implicitly covers the whole attachment; do the same here now that the pipeline
        // declared viewport and scissor dynamic.
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(extent.width);
        viewport.height = static_cast<float>(extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(m_data->cmdBuffer, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = { 0, 0 };
        scissor.extent = extent;
        vkCmdSetScissor(m_data->cmdBuffer, 0, 1, &scissor);
    }

    void VulkanCommandBuffer::setRenderPipeline(std::shared_ptr<Pipeline> pipeline)
    {
        if (!m_data->renderPassActive) {
            throw std::runtime_error("setRenderPipeline: no active render pass");
        }
        m_data->pipeline = static_cast<VulkanPipeline*>(pipeline.get());
        vkCmdBindPipeline(m_data->cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_data->pipeline->pipeline());
    }

    void VulkanCommandBuffer::setVertexBuffer(
        uint32_t slot, const std::shared_ptr<GBuffer>& buffer, size_t offset)
    {
        if (!buffer) return;
        if (slot >= kMaxBindingSlots) {
            throw std::runtime_error("setVertexBuffer: slot is outside the supported range");
        }
        auto* vulkanBuffer = static_cast<VulkanBuffer*>(buffer.get());
        m_data->bindings[slot] = BoundBuffer{
            vulkanBuffer,
            vulkanBuffer->currentRegionOffset() + offset,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
        };
    }

    void VulkanCommandBuffer::setUniformBuffer(
        uint32_t slot, ShaderStage stage, const std::shared_ptr<GBuffer>& buffer, size_t offset)
    {
        if (!buffer) return;
        if (slot >= kMaxBindingSlots) {
            throw std::runtime_error("setUniformBuffer: slot is outside the supported range");
        }
        if (stage == ShaderStage::Compute) {
            throw std::runtime_error("setUniformBuffer: compute stage is not valid inside a render pass");
        }
        // A single descriptor binding already covers both the vertex and the fragment stage, so
        // the two calls the application makes for slot 1 collapse into one write. Metal needs the
        // pair because it binds per stage; here the second call is simply idempotent.
        auto* vulkanBuffer = static_cast<VulkanBuffer*>(buffer.get());
        m_data->bindings[slot] = BoundBuffer{
            vulkanBuffer,
            vulkanBuffer->currentRegionOffset() + offset,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
        };
    }

    void VulkanCommandBuffer::flushDescriptorSet()
    {
        if (!m_data->pipeline) {
            throw std::runtime_error("draw: no pipeline is bound");
        }

        VkDevice logicalDevice = m_data->device->logicalDevice();
        VkDescriptorSetLayout layout = m_data->pipeline->descriptorSetLayout();

        // A fresh set per draw call. Anything else would have to track that third-party code —
        // the ImGui backend, for one — binds its own sets to the same slot between our draws.
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_data->queue->currentDescriptorPool();
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &layout;

        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        VkCheck(vkAllocateDescriptorSets(logicalDevice, &allocInfo, &descriptorSet),
                "vkAllocateDescriptorSets (raise kDescriptorSetsPerFrame if the pool ran out)");

        std::vector<VkDescriptorBufferInfo> bufferInfos;
        std::vector<VkWriteDescriptorSet> writes;
        bufferInfos.reserve(kMaxBindingSlots);
        writes.reserve(kMaxBindingSlots);

        for (uint32_t slot = 0; slot < kMaxBindingSlots; ++slot) {
            const BoundBuffer& bound = m_data->bindings[slot];
            if (!bound.buffer) continue;

            VkDescriptorBufferInfo info{};
            info.buffer = bound.buffer->buffer();
            info.offset = bound.offset;
            info.range = bound.buffer->size();
            bufferInfos.push_back(info);

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptorSet;
            write.dstBinding = slot;
            write.dstArrayElement = 0;
            write.descriptorCount = 1;
            write.descriptorType = bound.descriptorType;
            writes.push_back(write);
        }

        // pBufferInfo is filled only now: the vector reallocates while it grows, so pointers
        // taken during the loop above could dangle.
        for (size_t i = 0; i < writes.size(); ++i) {
            writes[i].pBufferInfo = &bufferInfos[i];
        }

        if (!writes.empty()) {
            vkUpdateDescriptorSets(logicalDevice, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }

        vkCmdBindDescriptorSets(m_data->cmdBuffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_data->pipeline->pipelineLayout(),
                                0, 1, &descriptorSet,
                                0, nullptr);
    }

    void VulkanCommandBuffer::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
    {
        if (!m_data->renderPassActive) throw std::runtime_error("draw: no active render pass");
        flushDescriptorSet();
        vkCmdDraw(m_data->cmdBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void VulkanCommandBuffer::drawIndexed(const std::shared_ptr<GBuffer>& indexBuffer,
        IndexType indexType,
        uint32_t indexCount,
        uint32_t instanceCount,
        uint32_t firstIndexOffsetBytes,
        int32_t vertexOffset,
        uint32_t firstInstance)
    {
        if (!m_data->renderPassActive) throw std::runtime_error("drawIndexed: no active render pass");
        if (!indexBuffer) return;

        flushDescriptorSet();

        auto* vulkanIndexBuffer = static_cast<VulkanBuffer*>(indexBuffer.get());
        const VkIndexType vkIndexType =
            (indexType == IndexType::UInt16) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

        // The interface expresses the start of the draw as a *byte* offset, the way Metal's
        // indexBufferOffset does, while vkCmdDrawIndexed counts whole indices. Folding the byte
        // offset into the bind keeps the two backends exactly equivalent, including offsets that
        // are not a multiple of the index size.
        vkCmdBindIndexBuffer(m_data->cmdBuffer,
                             vulkanIndexBuffer->buffer(),
                             vulkanIndexBuffer->currentRegionOffset() + firstIndexOffsetBytes,
                             vkIndexType);

        vkCmdDrawIndexed(m_data->cmdBuffer, indexCount, instanceCount, 0, vertexOffset, firstInstance);
    }

    void VulkanCommandBuffer::endRenderPass()
    {
        if (!m_data->renderPassActive) {
            throw std::runtime_error("endRenderPass: no active render pass");
        }
        vkCmdEndRenderPass(m_data->cmdBuffer);
        m_data->renderPassActive = false;
        m_data->pipeline = nullptr;
    }

    void VulkanCommandBuffer::present(const std::shared_ptr<GImage>& image)
    {
        if (!image) return;
        auto* vulkanImage = static_cast<VulkanImage*>(image.get());
        m_data->presentSwapChain = vulkanImage->swapChain();
        m_data->presentImageIndex = vulkanImage->imageIndex();
    }

    void VulkanCommandBuffer::commit()
    {
        if (m_data->committed) return;
        if (m_data->renderPassActive) {
            throw std::runtime_error("commit: endRenderPass() was never called");
        }

        VkCheck(vkEndCommandBuffer(m_data->cmdBuffer), "vkEndCommandBuffer");

        VkDevice logicalDevice = m_data->device->logicalDevice();
        const VkFence fence = m_data->queue->currentFence();

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &m_data->cmdBuffer;

        VkSemaphore waitSemaphore = VK_NULL_HANDLE;
        VkSemaphore signalSemaphore = VK_NULL_HANDLE;
        constexpr VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

        if (m_data->presentSwapChain) {
            // Do not write the image before the presentation engine releases it, and let the
            // present wait until this submit is finished with it.
            waitSemaphore = m_data->presentSwapChain->currentImageAvailableSemaphore();
            signalSemaphore = m_data->presentSwapChain->renderFinishedSemaphore(m_data->presentImageIndex);

            submitInfo.waitSemaphoreCount = 1;
            submitInfo.pWaitSemaphores = &waitSemaphore;
            submitInfo.pWaitDstStageMask = &waitStage;
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = &signalSemaphore;
        }

        VkCheck(vkResetFences(logicalDevice, 1, &fence), "vkResetFences");
        VkCheck(vkQueueSubmit(m_data->queue->graphicsQueue(), 1, &submitInfo, fence), "vkQueueSubmit");

        if (m_data->presentSwapChain) {
            m_data->presentSwapChain->present(m_data->presentImageIndex, signalSemaphore);
        }

        m_data->committed = true;
        m_data->queue->endFrame();
    }

    void* VulkanCommandBuffer::nativeHandle()
    {
        return (void*)m_data->cmdBuffer;
    }

    void* VulkanCommandBuffer::nativeEncoder()
    {
        return m_data->renderPassActive ? (void*)m_data->cmdBuffer : nullptr;
    }

}
