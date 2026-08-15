//
// Created by Artem Avdoshkin on 12.07.2025.
//

#ifndef RENDERING_VULKANCOMMANDQUEUE_HPP
#define RENDERING_VULKANCOMMANDQUEUE_HPP

#include <vulkan/vulkan.h>

#include "CommandQueue.hpp"
#include "Device.hpp"

namespace dmrender
{
    struct VulkanCommandQueuesData;

    /**
     * @class VulkanCommandQueues
     * @brief Owns everything that is recycled once per frame slot.
     *
     * A Metal command queue hands out fresh, autoreleased command buffers and the driver
     * recycles them. Vulkan makes the recycling explicit, so this class keeps @c kFramesInFlight
     * copies of every per-frame resource and cycles through them:
     *
     *  | resource            | why it is per frame slot                                  |
     *  |---------------------|-----------------------------------------------------------|
     *  | VkCommandBuffer     | cannot be re-recorded while the GPU still executes it      |
     *  | VkFence             | tells the CPU when that slot became free again             |
     *  | VkDescriptorPool    | reset wholesale instead of freeing individual sets         |
     *
     * `beginFrame()` blocks until the slot is free and resets it; `endFrame()` advances to the
     * next slot after a successful submit. Both are driven by the swapchain and the command
     * buffer, so application code never sees them.
     */
    class VulkanCommandQueues : public CommandQueue {
    public:
        explicit VulkanCommandQueues(const std::shared_ptr<Device>& device);
        ~VulkanCommandQueues() override;

        /**
         * @brief Returns a CommandBuffer recording into the current frame slot.
         * @note Recording is already open when this returns, mirroring Metal where
         *       `[queue commandBuffer]` is immediately usable.
         */
        std::shared_ptr<CommandBuffer> getCommandBuffer() override;

        /// @return Pointer to the VkCommandPool handle.
        void* nativeHandle() const override;
        void* getGraphicsQueue() const;
        void* getPresentQueue() const;

        // --- Frame lifecycle, driven by VulkanSwapChain and VulkanCommandBuffer ---

        /// @brief Waits for the current slot to be free, then resets its command buffer and pool.
        void beginFrame();

        /// @brief Advances to the next frame slot. Called after a successful submit + present.
        void endFrame();

        uint32_t currentFrameSlot() const;
        VkCommandBuffer currentCommandBuffer() const;
        VkFence currentFence() const;
        VkDescriptorPool currentDescriptorPool() const;

        /**
         * @brief Looks up a descriptor set built earlier this frame from identical bindings.
         *
         * Allocating and writing a descriptor set per draw call is the single largest CPU cost
         * in this backend. Most draws in a frame rebind the same handful of resources, so the
         * sets are cached by a hash of exactly what was bound and reused. The cache is cleared
         * whenever the frame slot's pool is reset, which is what makes reuse safe.
         *
         * @param key Hash of the bound state, from VulkanCommandBuffer.
         * @return The cached set, or VK_NULL_HANDLE if this frame has not built it yet.
         */
        VkDescriptorSet findCachedDescriptorSet(uint64_t key) const;

        /// @brief Records a set against @p key for the rest of this frame.
        void cacheDescriptorSet(uint64_t key, VkDescriptorSet set);

        /// @brief Descriptor sets served from the cache this frame, and total requested.
        void descriptorCacheStats(uint32_t& hits, uint32_t& requests) const;

        VkQueue graphicsQueue() const;
        VkQueue presentQueue() const;
        VkCommandPool commandPool() const;

    private:
        std::unique_ptr<VulkanCommandQueuesData> m_data;
    };
}
#endif //RENDERING_VULKANCOMMANDQUEUE_HPP
