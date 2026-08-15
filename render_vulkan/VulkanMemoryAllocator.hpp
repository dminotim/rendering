//
// Created by Artem Avdoshkin on 15.08.2025.
//

#ifndef RENDERING_VULKANMEMORYALLOCATOR_HPP
#define RENDERING_VULKANMEMORYALLOCATOR_HPP

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>

namespace dmrender {

    struct VulkanMemoryAllocatorData;

    /**
     * @struct VulkanAllocation
     * @brief A suballocated range of device memory.
     *
     * Binding a resource takes `memory` and `offset` together — the offset is no longer zero the
     * way it was when every resource owned its own VkDeviceMemory.
     */
    struct VulkanAllocation {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;

        /**
         * @brief Pointer to this allocation's first byte, or nullptr if the memory is not mapped.
         *
         * Blocks of host-visible memory are mapped once when created and stay mapped, so this is
         * the block's mapping already advanced by `offset`. Never call vkMapMemory on it.
         */
        void* mapped = nullptr;

        VkMemoryPropertyFlags properties = 0;

        /// Index of the owning block, or kDedicated when this allocation owns its VkDeviceMemory.
        uint32_t blockIndex = 0;

        bool isValid() const { return memory != VK_NULL_HANDLE; }
    };

    /**
     * @class VulkanMemoryAllocator
     * @brief Suballocates device memory out of a small number of large blocks.
     *
     * Vulkan drivers cap how many times `vkAllocateMemory` may be called — commonly 4096, and it
     * is a hard limit rather than a performance hint. One allocation per resource therefore puts
     * a low ceiling on how many resources can exist at once, and each call is slow besides. This
     * class allocates a handful of large blocks and hands out ranges within them, which is what
     * every production Vulkan renderer does (usually via AMD's VMA).
     *
     * @par Strategy
     * Blocks are pooled per memory type. Within a block, free ranges are kept sorted by offset
     * and satisfied first-fit; freeing inserts the range back and coalesces with its neighbours,
     * so a long-lived block does not fragment into dust under create/destroy churn.
     *
     * @par Linear and optimal resources never share a block
     * Vulkan requires padding between a linear resource (a buffer) and an optimal-tiling one (an
     * image) when they share memory, governed by `bufferImageGranularity`. Rather than track that
     * per range, blocks are typed: buffers come from linear blocks and images from optimal ones.
     * This costs a little memory and removes an entire category of subtle corruption.
     *
     * @par Dedicated allocations
     * A request larger than half a block gets its own VkDeviceMemory. Suballocating it would
     * strand the remainder of a fresh block, and such allocations are rare enough that spending
     * one of the driver's allocation slots is the right trade.
     *
     * @note Not thread-safe, in line with the rest of the backend.
     */
    class VulkanMemoryAllocator {
    public:
        /// @brief Whether a resource uses linear or optimal tiling. See the class documentation.
        enum class ResourceKind { Linear, Optimal };

        VulkanMemoryAllocator(VkDevice device,
                              const VkPhysicalDeviceMemoryProperties& memoryProperties,
                              VkDeviceSize bufferImageGranularity);

        ~VulkanMemoryAllocator();

        VulkanMemoryAllocator(const VulkanMemoryAllocator&) = delete;
        VulkanMemoryAllocator& operator=(const VulkanMemoryAllocator&) = delete;

        /**
         * @brief Suballocates memory satisfying @p requirements.
         * @param requirements The resource's VkMemoryRequirements.
         * @param memoryTypeIndex The memory type to allocate from.
         * @param kind Whether the resource is linear (buffer) or optimal (image).
         * @return A valid allocation; throws std::runtime_error on failure.
         */
        VulkanAllocation allocate(const VkMemoryRequirements& requirements,
                                  uint32_t memoryTypeIndex,
                                  ResourceKind kind);

        /// @brief Returns an allocation to its block, or frees it if it was dedicated.
        void free(const VulkanAllocation& allocation);

        /// @brief Aggregate figures, useful for reporting and for spotting leaks.
        struct Stats {
            uint32_t blockCount = 0;        ///< Live VkDeviceMemory objects backing pools.
            uint32_t dedicatedCount = 0;    ///< Live VkDeviceMemory objects owned by one resource.
            uint64_t reservedBytes = 0;     ///< Total memory held, including free space in blocks.
            uint64_t usedBytes = 0;         ///< Total handed out to live allocations.
            uint32_t liveAllocations = 0;
        };
        Stats stats() const;

    private:
        std::unique_ptr<VulkanMemoryAllocatorData> m_data;
    };

} // namespace dmrender

#endif //RENDERING_VULKANMEMORYALLOCATOR_HPP
