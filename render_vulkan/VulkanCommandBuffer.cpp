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
#include <render_vulkan/VulkanSampler.hpp>
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

    /// @brief One remembered `setTexture` call.
    struct BoundTexture
    {
        VkImageView imageView = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    };

    struct VulkanCommandBufferData
    {
        std::shared_ptr<CommandQueue> queueRef;   ///< Keeps the queue alive while recording.
        VulkanCommandQueues* queue = nullptr;
        VulkanDevice* device = nullptr;
        VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;

        VulkanPipeline* pipeline = nullptr;
        std::array<BoundBuffer, kMaxBindingSlots> bindings{};
        std::array<BoundTexture, kMaxTextureSlots> textures{};

        bool renderPassActive = false;
        bool committed = false;

        /// Filled by present(): which swapchain image this frame ends up on.
        VulkanSwapChain* presentSwapChain = nullptr;
        uint32_t presentImageIndex = 0;
    };

    namespace {

        /// FNV-1a, mixed one 64-bit value at a time.
        void hashCombine(uint64_t& hash, uint64_t value)
        {
            hash ^= value;
            hash *= 1099511628211ull;
        }

        constexpr uint64_t kHashSeed = 14695981039346656037ull;

        uint64_t hashBufferBindings(const std::array<BoundBuffer, kMaxBindingSlots>& bindings,
                                    VkDescriptorSetLayout layout)
        {
            // The layout is part of the key because two pipelines with different layouts cannot
            // share a set even when the same resources are bound.
            uint64_t hash = kHashSeed;
            hashCombine(hash, reinterpret_cast<uint64_t>(layout));
            for (uint32_t slot = 0; slot < kMaxBindingSlots; ++slot) {
                const BoundBuffer& bound = bindings[slot];
                if (!bound.buffer) continue;
                hashCombine(hash, slot);
                hashCombine(hash, reinterpret_cast<uint64_t>(bound.buffer->buffer()));
                hashCombine(hash, static_cast<uint64_t>(bound.offset));
                hashCombine(hash, static_cast<uint64_t>(bound.buffer->size()));
                hashCombine(hash, static_cast<uint64_t>(bound.descriptorType));
            }
            return hash;
        }

        uint64_t hashTextureBindings(const std::array<BoundTexture, kMaxTextureSlots>& textures,
                                     VkDescriptorSetLayout layout)
        {
            uint64_t hash = kHashSeed;
            hashCombine(hash, reinterpret_cast<uint64_t>(layout));
            for (uint32_t slot = 0; slot < kMaxTextureSlots; ++slot) {
                const BoundTexture& bound = textures[slot];
                if (!bound.imageView) continue;
                hashCombine(hash, slot);
                hashCombine(hash, reinterpret_cast<uint64_t>(bound.imageView));
                hashCombine(hash, reinterpret_cast<uint64_t>(bound.sampler));
                hashCombine(hash, static_cast<uint64_t>(bound.layout));
            }
            return hash;
        }

    } // namespace

    VulkanCommandBuffer::VulkanCommandBuffer(const std::shared_ptr<CommandQueue>& cmdQueue)
        : m_data(std::make_unique<VulkanCommandBufferData>())
    {
        if (!cmdQueue) throw std::runtime_error("VulkanCommandBuffer: null command queue");

        m_data->queueRef = cmdQueue;
        m_data->queue = static_cast<VulkanCommandQueues*>(cmdQueue.get());
        m_data->device = static_cast<VulkanDevice*>(cmdQueue->getDevice().get());
        // Open the frame slot before touching its command buffer. acquireNextImage() normally got
        // here first, and this call is then a no-op — waiting on an already signalled fence
        // returns immediately and resetting a command buffer in the initial state is legal. It
        // matters for the case where there is no acquire at all: a command buffer that renders
        // only into offscreen targets and is never presented still needs its slot recycled.
        m_data->queue->beginFrame();
        m_data->cmdBuffer = m_data->queue->currentCommandBuffer();

        // Open recording right away so the object behaves like a freshly created MTLCommandBuffer.
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
        const uint32_t colorCount = descriptor->colorAttachmentCount();
        if (colorCount == 0) {
            throw std::runtime_error("beginRenderPass: no colour attachment was set");
        }

        // Walk every colour attachment once, building the render pass key, the framebuffer's
        // view list and the clear values together. Each attachment contributes its own format,
        // load op and resting layout, so a pass may mix a swapchain image with offscreen targets.
        // The render pass declares attachments as colours, then resolve targets, then depth, so
        // the view list and the clear values are gathered in that same order.
        RenderPassKey key{};
        std::vector<VkImageView> attachmentViews;
        std::vector<VkImageView> resolveViews;
        std::vector<VkClearValue> clearValues;
        attachmentViews.reserve(colorCount * 2 + 1);
        clearValues.reserve(colorCount * 2 + 1);

        VkExtent2D extent{ 0, 0 };
        SampleCount sampleCount = SampleCount::One;

        for (uint32_t index = 0; index < colorCount; ++index) {
            const auto& color = descriptor->colorAttachment(index);
            if (!color.isValid()) {
                throw std::runtime_error("beginRenderPass: colour attachments must be set from 0 upwards without gaps");
            }

            auto* image = static_cast<VulkanImage*>(color.image.get());

            if (index == 0) {
                extent = VkExtent2D{ image->width(), image->height() };
                sampleCount = image->sampleCount();
            } else if (image->width() != extent.width || image->height() != extent.height) {
                throw std::runtime_error("beginRenderPass: all attachments must have the same size");
            } else if (image->sampleCount() != sampleCount) {
                throw std::runtime_error("beginRenderPass: all attachments must have the same sample count");
            }

            const std::shared_ptr<GImage>& resolve = descriptor->resolveAttachment(index);
            if (resolve && image->sampleCount() == SampleCount::One) {
                throw std::runtime_error(
                    "beginRenderPass: a resolve target was set on a single-sample attachment");
            }

            RenderPassAttachmentKey colorKey{};
            colorKey.format = ToVkFormat(image->format());
            colorKey.clear = color.clear;
            colorKey.restingLayout = image->restingLayout();
            if (resolve) {
                auto* resolveImage = static_cast<VulkanImage*>(resolve.get());
                colorKey.hasResolve = true;
                colorKey.resolveRestingLayout = resolveImage->restingLayout();
                resolveViews.push_back(resolveImage->imageView());
            }
            key.colors.push_back(colorKey);

            attachmentViews.push_back(image->imageView());

            VkClearValue clear{};
            clear.color = { { color.clearValue.color[0], color.clearValue.color[1],
                              color.clearValue.color[2], color.clearValue.color[3] } };
            clearValues.push_back(clear);
        }

        key.samples = ToVkSampleCount(sampleCount);

        // Resolve targets occupy the attachment slots immediately after the colours, and their
        // clear values are never used but must still be present to keep the indices aligned.
        for (VkImageView resolveView : resolveViews) {
            attachmentViews.push_back(resolveView);
            clearValues.push_back(VkClearValue{});
        }

        const auto& depth = descriptor->depthStencilAttachment();
        if (depth.isValid()) {
            auto* depthImage = static_cast<VulkanImage*>(depth.image.get());
            key.depth = RenderPassAttachmentKey{
                ToVkFormat(depthImage->format()), depth.clear, depthImage->restingLayout() };
            attachmentViews.push_back(depthImage->imageView());

            VkClearValue depthClear{};
            depthClear.depthStencil = { depth.clearValue.depth, depth.clearValue.stencil };
            clearValues.push_back(depthClear);
        }

        const VkRenderPass renderPass = m_data->device->acquireRenderPass(key);
        descriptor->setResolvedRenderPass(renderPass);
        const VkFramebuffer framebuffer = m_data->device->acquireFramebuffer(
            renderPass, attachmentViews, extent.width, extent.height);

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

    void VulkanCommandBuffer::setTexture(uint32_t slot,
                                         ShaderStage stage,
                                         const std::shared_ptr<GImage>& image,
                                         const std::shared_ptr<GSampler>& sampler)
    {
        if (!image || !sampler) return;
        if (slot >= kMaxTextureSlots) {
            throw std::runtime_error("setTexture: slot is outside the supported range");
        }
        if (stage == ShaderStage::Compute) {
            throw std::runtime_error("setTexture: compute stage is not valid inside a render pass");
        }

        auto* vulkanImage = static_cast<VulkanImage*>(image.get());
        if (!hasFlag(vulkanImage->usage(), ImageUsage::Sampled)) {
            throw std::runtime_error("setTexture: image was not created with ImageUsage::Sampled");
        }

        auto* vulkanSampler = static_cast<VulkanSampler*>(sampler.get());

        // The image is expected to be in its resting layout, which for a sampled colour target is
        // SHADER_READ_ONLY_OPTIMAL. Whatever render pass last wrote it left it there, and that
        // pass's outgoing external dependency made the writes visible to this fragment shader,
        // so no barrier is needed here. One descriptor binding covers both stages, so the vertex
        // and fragment calls for the same slot collapse into one write.
        m_data->textures[slot] = BoundTexture{
            vulkanImage->imageView(),
            vulkanSampler->sampler(),
            vulkanImage->restingLayout()
        };
    }

    void VulkanCommandBuffer::setPushConstants(ShaderStage stage,
                                               const void* data,
                                               size_t size,
                                               size_t offset)
    {
        if (!data || size == 0) return;
        if (!m_data->pipeline) {
            throw std::runtime_error("setPushConstants: no pipeline is bound");
        }
        if (offset + size > kMaxPushConstantBytes) {
            throw std::runtime_error(
                "setPushConstants: writes past the guaranteed " +
                std::to_string(kMaxPushConstantBytes) + " byte push constant block");
        }
        if (stage == ShaderStage::Compute) {
            throw std::runtime_error("setPushConstants: compute stage is not valid inside a render pass");
        }

        // The pipeline layout declares one range spanning both stages, so the same bytes are
        // visible to whichever stage the shader reads them from. Passing the stage through would
        // require matching ranges exactly, and gains nothing at this size.
        vkCmdPushConstants(m_data->cmdBuffer,
                           m_data->pipeline->pipelineLayout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           static_cast<uint32_t>(offset),
                           static_cast<uint32_t>(size),
                           data);
    }

    void VulkanCommandBuffer::flushDescriptorSet()
    {
        if (!m_data->pipeline) {
            throw std::runtime_error("draw: no pipeline is bound");
        }

        VkDevice logicalDevice = m_data->device->logicalDevice();
        VkDescriptorPool pool = m_data->queue->currentDescriptorPool();

        // Fresh sets per draw call. Anything else would have to track that third-party code —
        // the ImGui backend, for one — binds its own sets to the same slots between our draws.
        auto allocate = [&](VkDescriptorSetLayout layout) {
            VkDescriptorSetAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = pool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts = &layout;

            VkDescriptorSet set = VK_NULL_HANDLE;
            VkCheck(vkAllocateDescriptorSets(logicalDevice, &allocInfo, &set),
                    "vkAllocateDescriptorSets (raise kDescriptorSetsPerFrame if the pool ran out)");
            return set;
        };

        // Hash exactly what would be written, so an identical binding state anywhere else in the
        // frame reuses the set instead of allocating and rewriting one. Draws that only differ
        // by push constants — the common case once those exist — collapse onto a single set.
        const uint64_t bufferKey = hashBufferBindings(m_data->bindings,
                                                      m_data->pipeline->bufferSetLayout());
        const uint64_t textureKey = hashTextureBindings(m_data->textures,
                                                        m_data->pipeline->textureSetLayout());

        VkDescriptorSet cachedBufferSet = m_data->queue->findCachedDescriptorSet(bufferKey);
        VkDescriptorSet cachedTextureSet = m_data->queue->findCachedDescriptorSet(textureKey);

        // Both info arrays are sized up front and never grow, so the addresses handed to
        // VkWriteDescriptorSet stay valid until vkUpdateDescriptorSets consumes them.
        std::array<VkDescriptorBufferInfo, kMaxBindingSlots> bufferInfos{};
        std::array<VkDescriptorImageInfo, kMaxTextureSlots> imageInfos{};
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(kMaxBindingSlots + kMaxTextureSlots);

        // --- set 0: buffers ---
        VkDescriptorSet bufferSet = cachedBufferSet;
        for (uint32_t slot = 0; slot < kMaxBindingSlots && !cachedBufferSet; ++slot) {
            const BoundBuffer& bound = m_data->bindings[slot];
            if (!bound.buffer) continue;

            if (!bufferSet) bufferSet = allocate(m_data->pipeline->bufferSetLayout());

            bufferInfos[slot].buffer = bound.buffer->buffer();
            bufferInfos[slot].offset = bound.offset;
            bufferInfos[slot].range = bound.buffer->size();

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = bufferSet;
            write.dstBinding = slot;
            write.dstArrayElement = 0;
            write.descriptorCount = 1;
            write.descriptorType = bound.descriptorType;
            write.pBufferInfo = &bufferInfos[slot];
            writes.push_back(write);
        }

        // --- set 1: textures ---
        VkDescriptorSet textureSet = cachedTextureSet;
        for (uint32_t slot = 0; slot < kMaxTextureSlots && !cachedTextureSet; ++slot) {
            const BoundTexture& bound = m_data->textures[slot];
            if (!bound.imageView) continue;

            if (!textureSet) textureSet = allocate(m_data->pipeline->textureSetLayout());

            imageInfos[slot].imageView = bound.imageView;
            imageInfos[slot].sampler = bound.sampler;
            imageInfos[slot].imageLayout = bound.layout;

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = textureSet;
            write.dstBinding = slot;
            write.dstArrayElement = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &imageInfos[slot];
            writes.push_back(write);
        }

        if (!writes.empty()) {
            vkUpdateDescriptorSets(logicalDevice, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }

        // Only newly built sets are recorded; a cache hit already has its entry.
        if (bufferSet && !cachedBufferSet) m_data->queue->cacheDescriptorSet(bufferKey, bufferSet);
        if (textureSet && !cachedTextureSet) m_data->queue->cacheDescriptorSet(textureKey, textureSet);

        // Binding still happens every draw even on a cache hit: third-party code, the ImGui
        // backend in particular, binds its own sets between ours. Binding is cheap; allocating
        // and writing is what the cache removes.
        VkPipelineLayout pipelineLayout = m_data->pipeline->pipelineLayout();
        if (bufferSet) {
            vkCmdBindDescriptorSets(m_data->cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                    kBufferDescriptorSet, 1, &bufferSet, 0, nullptr);
        }
        if (textureSet) {
            vkCmdBindDescriptorSets(m_data->cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                    kTextureDescriptorSet, 1, &textureSet, 0, nullptr);
        }
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
        if (!vulkanImage->swapChain()) {
            // An offscreen texture is a perfectly good render target but it has nothing to
            // present to. Silently ignoring this would show an unchanged window and look like a
            // rendering bug rather than the API misuse it is.
            throw std::runtime_error("present: image did not come from a swapchain");
        }
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
