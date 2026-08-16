//
// Created by Artem Avdoshkin on 14.08.2025.
//

#ifndef RENDERING_VULKANBUFFER_HPP
#define RENDERING_VULKANBUFFER_HPP

#include <vulkan/vulkan.h>

#include <memory>

#include "GBuffer.hpp"

namespace dmrender {

    class VulkanDevice;
    struct VulkanBufferNativeData;

    /**
     * @class VulkanBuffer
     * @brief GBuffer whose memory placement follows its BufferUsage hint.
     *
     * @par Where the memory goes
     * - **Static** asks for DEVICE_LOCAL memory (VRAM). On a discrete GPU that memory is not
     *   CPU-addressable, so the initial contents and any later `update()` travel through the
     *   device's staging buffer and a `vkCmdCopyBuffer`.
     * - **Dynamic** and **Stream** ask for HOST_VISIBLE|HOST_COHERENT memory kept mapped for the
     *   buffer's lifetime, matching Metal's shared-storage MTLBuffer: `update()` is a memcpy.
     *
     * Two things can change that outcome, and `memoryLocation()` reports what actually happened.
     * If the device-local budget cannot fit the allocation, a Static buffer falls back to
     * host-visible memory instead of failing. And if the chosen device-local type turns out to be
     * host-visible too — integrated GPUs, or discrete ones with resizable BAR — the buffer is
     * mapped directly and the staging copy is skipped even though it lives in VRAM.
     *
     * @par Dynamic buffers
     * A BufferUsage::Dynamic buffer is allocated as @c kFramesInFlight consecutive, properly
     * aligned regions inside one VkBuffer. `update()` always writes the region belonging to the
     * frame slot currently being recorded, and the command buffer binds that same region. Without
     * this a per-frame uniform update would overwrite memory that a still-executing frame is
     * reading.
     *
     * @par Partial updates
     * Regions make a partial write (`offset > 0` or `dataSize < size()`) subtler than it looks:
     * the bytes the caller did *not* write would otherwise still hold whatever that region
     * contained a frame ago, not the current contents. So a partial write first copies the whole
     * of the most recently written region into this one, then applies the new bytes on top,
     * leaving every region a complete snapshot. The copy is skipped when the write covers the
     * whole buffer, and when the region is already the newest — so the common case of rewriting
     * a whole uniform struct each frame costs nothing extra.
     */
    class VulkanBuffer : public GBuffer {
    public:
        VulkanBuffer(VulkanDevice* device,
                     BufferType type,
                     BufferUsage usage,
                     size_t size,
                     const void* initialData,
                     const std::string& debugName);

        ~VulkanBuffer() override;

        BufferType type() const override;
        BufferUsage usage() const override;
        size_t size() const override;
        MemoryLocation memoryLocation() const override;

        void update(const void* data, size_t dataSize, size_t offset = 0) override;
        void readback(void* destination, size_t destinationSize, size_t offset = 0) override;

        void* nativeHandle() const override;

        const std::string& debugName() const override;
        void setDebugName(const std::string& name) override;

        /// @brief The underlying VkBuffer (spans every region).
        VkBuffer buffer() const;

        /**
         * @brief The VkBufferUsageFlags the allocation was created with.
         *
         * Checked before a descriptor is written: a pipeline may declare a slot as a storage
         * buffer while the buffer bound there was created without STORAGE_BUFFER_BIT, and that
         * combination has to be reported rather than left to produce undefined results.
         */
        VkBufferUsageFlags usageFlags() const;

        /**
         * @brief Byte offset of the region that should be bound for the frame being recorded.
         * @return 0 for static buffers, `frameSlot * regionStride` for dynamic ones.
         */
        VkDeviceSize currentRegionOffset() const;

    private:
        /**
         * @brief Writes into one specific region rather than the current frame's.
         *
         * Used by the constructor to seed every region of a dynamic buffer, and by the public
         * update() once it has resolved which region the current frame owns.
         */
        void update(const void* data, size_t dataSize, size_t offset, uint32_t region);

        std::unique_ptr<VulkanBufferNativeData> m_data;
    };

} // namespace dmrender

#endif //RENDERING_VULKANBUFFER_HPP
