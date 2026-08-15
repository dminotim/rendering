#include "VulkanMemoryAllocator.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

#include <render_vulkan/VulkanUtils.hpp>

namespace dmrender {

    namespace {

        /// Size of a pooled block. Large enough that most scenes need only a handful.
        constexpr VkDeviceSize kBlockSize = 64ull * 1024 * 1024;

        /// Requests at or above this get their own VkDeviceMemory; see the class documentation.
        constexpr VkDeviceSize kDedicatedThreshold = kBlockSize / 2;

        /// Marks an allocation that owns its VkDeviceMemory rather than living inside a block.
        constexpr uint32_t kDedicated = 0xFFFFFFFFu;

        VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
        {
            if (alignment == 0) return value;
            return (value + alignment - 1) & ~(alignment - 1);
        }

    } // namespace

    /// A contiguous span of unused bytes within a block.
    struct FreeRange {
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
    };

    struct MemoryBlock {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
        uint32_t memoryTypeIndex = 0;
        VkMemoryPropertyFlags properties = 0;
        VulkanMemoryAllocator::ResourceKind kind = VulkanMemoryAllocator::ResourceKind::Linear;
        /// Sorted by offset and always coalesced, so a fit is a single forward scan.
        std::vector<FreeRange> freeRanges;
        uint32_t liveAllocations = 0;
        VkDeviceSize usedBytes = 0;
    };

    struct VulkanMemoryAllocatorData {
        VkDevice device = VK_NULL_HANDLE;
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        VkDeviceSize bufferImageGranularity = 1;

        /// Index into this vector is the blockIndex carried by every VulkanAllocation.
        /// Entries are never erased so indices stay stable; an emptied block is reused.
        std::vector<MemoryBlock> blocks;

        uint32_t dedicatedCount = 0;
        uint64_t dedicatedBytes = 0;
    };

    VulkanMemoryAllocator::VulkanMemoryAllocator(VkDevice device,
                                                 const VkPhysicalDeviceMemoryProperties& memoryProperties,
                                                 VkDeviceSize bufferImageGranularity)
        : m_data(std::make_unique<VulkanMemoryAllocatorData>())
    {
        m_data->device = device;
        m_data->memoryProperties = memoryProperties;
        m_data->bufferImageGranularity = std::max<VkDeviceSize>(bufferImageGranularity, 1);
    }

    VulkanMemoryAllocator::~VulkanMemoryAllocator()
    {
        for (MemoryBlock& block : m_data->blocks) {
            if (!block.memory) continue;
            if (block.mapped) {
                vkUnmapMemory(m_data->device, block.memory);
                block.mapped = nullptr;
            }
            vkFreeMemory(m_data->device, block.memory, nullptr);
            block.memory = VK_NULL_HANDLE;
        }
        m_data->blocks.clear();
    }

    VulkanAllocation VulkanMemoryAllocator::allocate(const VkMemoryRequirements& requirements,
                                                     uint32_t memoryTypeIndex,
                                                     ResourceKind kind)
    {
        if (memoryTypeIndex >= m_data->memoryProperties.memoryTypeCount) {
            throw std::runtime_error("VulkanMemoryAllocator: memory type index out of range");
        }

        const VkMemoryPropertyFlags properties =
            m_data->memoryProperties.memoryTypes[memoryTypeIndex].propertyFlags;
        const bool hostVisible = (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;

        // Anything sharing a block must also respect bufferImageGranularity against its
        // neighbours. Blocks are already segregated by kind, so only the alignment the resource
        // itself asks for matters here.
        const VkDeviceSize alignment = std::max<VkDeviceSize>(requirements.alignment, 1);

        VulkanAllocation allocation{};
        allocation.properties = properties;

        // --- Dedicated path ---
        if (requirements.size >= kDedicatedThreshold) {
            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = requirements.size;
            allocInfo.memoryTypeIndex = memoryTypeIndex;

            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkCheck(vkAllocateMemory(m_data->device, &allocInfo, nullptr, &memory),
                    "vkAllocateMemory (dedicated)");

            void* mapped = nullptr;
            if (hostVisible) {
                VkCheck(vkMapMemory(m_data->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped),
                        "vkMapMemory (dedicated)");
            }

            allocation.memory = memory;
            allocation.offset = 0;
            allocation.size = requirements.size;
            allocation.mapped = mapped;
            allocation.blockIndex = kDedicated;

            ++m_data->dedicatedCount;
            m_data->dedicatedBytes += requirements.size;
            return allocation;
        }

        // --- Pooled path: first fit across existing blocks of the right type and kind ---
        for (uint32_t blockIndex = 0; blockIndex < m_data->blocks.size(); ++blockIndex) {
            MemoryBlock& block = m_data->blocks[blockIndex];
            if (!block.memory) continue;
            if (block.memoryTypeIndex != memoryTypeIndex || block.kind != kind) continue;

            for (size_t rangeIndex = 0; rangeIndex < block.freeRanges.size(); ++rangeIndex) {
                FreeRange& range = block.freeRanges[rangeIndex];
                const VkDeviceSize alignedOffset = alignUp(range.offset, alignment);
                const VkDeviceSize padding = alignedOffset - range.offset;
                if (range.size < padding || range.size - padding < requirements.size) {
                    continue;
                }

                const VkDeviceSize leadingRemainder = padding;
                const VkDeviceSize trailingRemainder = range.size - padding - requirements.size;

                // Rewrite this free range as whatever survives on either side of the allocation.
                if (leadingRemainder == 0 && trailingRemainder == 0) {
                    block.freeRanges.erase(block.freeRanges.begin() + rangeIndex);
                } else if (leadingRemainder == 0) {
                    range.offset = alignedOffset + requirements.size;
                    range.size = trailingRemainder;
                } else {
                    range.size = leadingRemainder;
                    if (trailingRemainder > 0) {
                        block.freeRanges.insert(
                            block.freeRanges.begin() + rangeIndex + 1,
                            FreeRange{ alignedOffset + requirements.size, trailingRemainder });
                    }
                }

                ++block.liveAllocations;
                block.usedBytes += requirements.size;

                allocation.memory = block.memory;
                allocation.offset = alignedOffset;
                allocation.size = requirements.size;
                allocation.mapped = block.mapped
                    ? static_cast<char*>(block.mapped) + alignedOffset
                    : nullptr;
                allocation.blockIndex = blockIndex;
                return allocation;
            }
        }

        // --- No room anywhere: create a block, reusing an emptied slot if one exists ---
        const VkDeviceSize blockSize = std::max(kBlockSize, requirements.size);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = blockSize;
        allocInfo.memoryTypeIndex = memoryTypeIndex;

        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkCheck(vkAllocateMemory(m_data->device, &allocInfo, nullptr, &memory), "vkAllocateMemory (block)");

        void* mapped = nullptr;
        if (hostVisible) {
            VkCheck(vkMapMemory(m_data->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped),
                    "vkMapMemory (block)");
        }

        MemoryBlock block{};
        block.memory = memory;
        block.size = blockSize;
        block.mapped = mapped;
        block.memoryTypeIndex = memoryTypeIndex;
        block.properties = properties;
        block.kind = kind;
        block.liveAllocations = 1;
        block.usedBytes = requirements.size;
        // The allocation takes the front of the block; the rest stays free.
        if (blockSize > requirements.size) {
            block.freeRanges.push_back(FreeRange{ requirements.size, blockSize - requirements.size });
        }

        uint32_t blockIndex = kDedicated;
        for (uint32_t i = 0; i < m_data->blocks.size(); ++i) {
            if (!m_data->blocks[i].memory) { blockIndex = i; break; }
        }
        if (blockIndex == kDedicated) {
            blockIndex = static_cast<uint32_t>(m_data->blocks.size());
            m_data->blocks.push_back(block);
        } else {
            m_data->blocks[blockIndex] = block;
        }

        allocation.memory = memory;
        allocation.offset = 0;
        allocation.size = requirements.size;
        allocation.mapped = mapped;
        allocation.blockIndex = blockIndex;
        return allocation;
    }

    void VulkanMemoryAllocator::free(const VulkanAllocation& allocation)
    {
        if (!allocation.isValid()) return;

        if (allocation.blockIndex == kDedicated) {
            if (allocation.mapped) {
                vkUnmapMemory(m_data->device, allocation.memory);
            }
            vkFreeMemory(m_data->device, allocation.memory, nullptr);
            --m_data->dedicatedCount;
            m_data->dedicatedBytes -= allocation.size;
            return;
        }

        if (allocation.blockIndex >= m_data->blocks.size()) return;
        MemoryBlock& block = m_data->blocks[allocation.blockIndex];
        if (!block.memory) return;

        // Insert the range back in offset order, then merge with whichever neighbours touch it.
        // Without the merge, repeated create/destroy of same-sized resources would still work but
        // a differently sized one would eventually find only slivers.
        FreeRange returned{ allocation.offset, allocation.size };
        auto position = std::lower_bound(
            block.freeRanges.begin(), block.freeRanges.end(), returned,
            [](const FreeRange& a, const FreeRange& b) { return a.offset < b.offset; });
        position = block.freeRanges.insert(position, returned);

        if (position + 1 != block.freeRanges.end()) {
            auto next = position + 1;
            if (position->offset + position->size == next->offset) {
                position->size += next->size;
                block.freeRanges.erase(next);
            }
        }
        if (position != block.freeRanges.begin()) {
            auto previous = position - 1;
            if (previous->offset + previous->size == position->offset) {
                previous->size += position->size;
                block.freeRanges.erase(position);
            }
        }

        --block.liveAllocations;
        block.usedBytes -= allocation.size;

        // An empty block is released so a long-running application does not hold a pool it has
        // stopped using. The slot stays in the vector so existing block indices remain valid.
        if (block.liveAllocations == 0) {
            if (block.mapped) {
                vkUnmapMemory(m_data->device, block.memory);
                block.mapped = nullptr;
            }
            vkFreeMemory(m_data->device, block.memory, nullptr);
            block.memory = VK_NULL_HANDLE;
            block.freeRanges.clear();
            block.size = 0;
            block.usedBytes = 0;
        }
    }

    VulkanMemoryAllocator::Stats VulkanMemoryAllocator::stats() const
    {
        Stats result{};
        result.dedicatedCount = m_data->dedicatedCount;
        result.reservedBytes = m_data->dedicatedBytes;
        result.usedBytes = m_data->dedicatedBytes;
        result.liveAllocations = m_data->dedicatedCount;

        for (const MemoryBlock& block : m_data->blocks) {
            if (!block.memory) continue;
            ++result.blockCount;
            result.reservedBytes += block.size;
            result.usedBytes += block.usedBytes;
            result.liveAllocations += block.liveAllocations;
        }
        return result;
    }

} // namespace dmrender
