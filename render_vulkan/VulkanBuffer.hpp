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
     * @brief GBuffer backed by a host visible, permanently mapped VkBuffer.
     *
     * Metal's shared-storage MTLBuffer can simply be written through its `contents` pointer.
     * The closest Vulkan equivalent is HOST_VISIBLE|HOST_COHERENT memory kept mapped for the
     * lifetime of the buffer, which is what this class does — no staging copies, no explicit
     * flushes, `update()` is a memcpy just like on Metal.
     *
     * @par Dynamic buffers
     * A BufferUsage::Dynamic buffer is allocated as @c kFramesInFlight consecutive, properly
     * aligned regions inside one VkBuffer. `update()` always writes the region belonging to the
     * frame slot currently being recorded, and the command buffer binds that same region. Without
     * this a per-frame uniform update would overwrite memory that a still-executing frame is
     * reading. Metal hides the same hazard behind its own buffer pooling.
     *
     * @note Because a dynamic update targets one region only, a *partial* update
     *       (`dataSize < size()`) leaves the other regions holding older data. Callers that
     *       rewrite the whole struct every frame — the normal uniform pattern — are unaffected.
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

        void update(const void* data, size_t dataSize, size_t offset = 0) override;

        void* nativeHandle() const override;

        const std::string& debugName() const override;
        void setDebugName(const std::string& name) override;

        /// @brief The underlying VkBuffer (spans every region).
        VkBuffer buffer() const;

        /**
         * @brief Byte offset of the region that should be bound for the frame being recorded.
         * @return 0 for static buffers, `frameSlot * regionStride` for dynamic ones.
         */
        VkDeviceSize currentRegionOffset() const;

    private:
        std::unique_ptr<VulkanBufferNativeData> m_data;
    };

} // namespace dmrender

#endif //RENDERING_VULKANBUFFER_HPP
