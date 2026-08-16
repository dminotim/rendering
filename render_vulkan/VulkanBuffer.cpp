#include "VulkanBuffer.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <render_vulkan/VulkanDevice.hpp>
#include <render_vulkan/VulkanUtils.hpp>

namespace dmrender {

    struct VulkanBufferNativeData
    {
        VulkanDevice* device = nullptr;
        VkBuffer buffer = VK_NULL_HANDLE;
        VulkanAllocation allocation{};
        void* mapped = nullptr;

        BufferType type = BufferType::Vertex;
        BufferUsage usage = BufferUsage::Static;
        size_t size = 0;            ///< Logical size the caller asked for.
        VkDeviceSize regionStride = 0;  ///< Distance between two per-frame regions.
        uint32_t regionCount = 1;
        MemoryLocation location = MemoryLocation::HostVisible;
        VkBufferUsageFlags usageFlags = 0;  ///< What roles this allocation can serve.
        /**
         * Which region currently holds the newest complete contents.
         *
         * A partial write only touches the bytes it was given, so the region it lands in is
         * only a valid snapshot if the *other* bytes were carried over from wherever the newest
         * copy lives. This tracks where that is.
         */
        uint32_t latestRegion = 0;
        std::string debugName;
    };

    namespace {

        VkBufferUsageFlags toBufferUsage(BufferType type)
        {
            switch (type) {
                case BufferType::Vertex:
                    // The vertex shader reads geometry through a storage buffer indexed by
                    // gl_VertexIndex, exactly like Metal reads `const device VertexData*`.
                    // VERTEX_BUFFER_BIT is kept so classic vkCmdBindVertexBuffers stays possible.
                    return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
                case BufferType::Index:
                    return VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
                case BufferType::Uniform:
                    // STORAGE as well: a pipeline may declare the slot this buffer is bound to as
                    // BufferBindingType::Storage, and the same allocation has to serve either
                    // role. The bit is free when unused.
                    return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                case BufferType::Storage:
                    return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                case BufferType::Indirect:
                    // STORAGE_BUFFER as well, so a compute shader could later fill the draw list
                    // in place without the buffer needing to be recreated.
                    return VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            }
            return 0;
        }

        /// @brief True when a buffer with this usage hint belongs in device-local memory.
        bool wantsDeviceLocal(BufferUsage usage)
        {
            // Only Static earns the staging copy: it is written once and read many times, so the
            // one-off upload cost is repaid by every subsequent GPU read. Dynamic and Stream are
            // rewritten by the CPU too often for that trade to pay off.
            return usage == BufferUsage::Static;
        }

        VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
        {
            if (alignment == 0) return value;
            return (value + alignment - 1) & ~(alignment - 1);
        }

    } // namespace

    VulkanBuffer::VulkanBuffer(VulkanDevice* device,
                               BufferType type,
                               BufferUsage usage,
                               size_t size,
                               const void* initialData,
                               const std::string& debugName)
        : m_data(std::make_unique<VulkanBufferNativeData>())
    {
        if (!device) throw std::runtime_error("VulkanBuffer: null device");
        if (size == 0) throw std::runtime_error("VulkanBuffer: zero sized buffer");

        m_data->device = device;
        m_data->type = type;
        m_data->usage = usage;
        m_data->size = size;
        m_data->debugName = debugName;
        m_data->regionCount = (usage == BufferUsage::Dynamic) ? kFramesInFlight : 1;

        const VkPhysicalDeviceLimits& limits = device->properties().limits;
        VkDeviceSize alignment = 16;
        // A uniform buffer may be bound to a slot the pipeline declares as storage, so its regions
        // have to satisfy whichever alignment is stricter.
        if (type == BufferType::Uniform) {
            alignment = std::max(limits.minUniformBufferOffsetAlignment,
                                 limits.minStorageBufferOffsetAlignment);
        }
        if (type == BufferType::Vertex || type == BufferType::Storage) {
            alignment = limits.minStorageBufferOffsetAlignment;
        }
        m_data->regionStride = alignUp(static_cast<VkDeviceSize>(size), alignment);

        const VkDeviceSize totalSize = m_data->regionStride * m_data->regionCount;

        // Decide where this buffer should live before creating it, because a device-local buffer
        // additionally needs TRANSFER_DST so the staging copy can target it.
        bool placeInDeviceLocal = wantsDeviceLocal(usage);
        if (placeInDeviceLocal) {
            // The capacity check: if the allocation would not fit in what the driver says is
            // still available, fall back to host-visible memory rather than failing outright.
            // Rendering a little slower beats not rendering at all.
            const MemoryBudget budget = device->queryMemoryBudget();
            if (budget.preciseBudget && budget.availableBytes() < totalSize) {
                placeInDeviceLocal = false;
            }
        }

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = totalSize;
        bufferInfo.usage = toBufferUsage(type);
        if (placeInDeviceLocal) {
            // DST so staging can write it, SRC so readback() can read it. Neither is expressible
            // through BufferType, and both are cheap to always allow.
            bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        }
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        m_data->usageFlags = bufferInfo.usage;

        VkDevice logicalDevice = device->logicalDevice();
        VkCheck(vkCreateBuffer(logicalDevice, &bufferInfo, nullptr, &m_data->buffer), "vkCreateBuffer");

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(logicalDevice, m_data->buffer, &requirements);

        // Ask for device-local, accept host-visible. On a discrete GPU the first choice wins and
        // the memory is not CPU-addressable; on an integrated one the same type is often both,
        // which the mapping check below turns into a free optimisation.
        constexpr VkMemoryPropertyFlags kHostVisible =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        VkMemoryPropertyFlags chosenFlags = 0;
        const uint32_t memoryTypeIndex = device->selectMemoryType(
            requirements.memoryTypeBits,
            placeInDeviceLocal ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : kHostVisible,
            kHostVisible,
            chosenFlags);

        // A buffer is linear-tiled, so it comes from a linear pool and can never end up adjacent
        // to an image inside the same block.
        m_data->allocation = device->allocator().allocate(
            requirements, memoryTypeIndex, VulkanMemoryAllocator::ResourceKind::Linear);
        VkCheck(vkBindBufferMemory(logicalDevice, m_data->buffer,
                                   m_data->allocation.memory, m_data->allocation.offset),
                "vkBindBufferMemory");

        m_data->location = (chosenFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            ? MemoryLocation::DeviceLocal
            : MemoryLocation::HostVisible;

        // The allocator maps whole host-visible blocks once and hands back a pointer already
        // offset to this allocation, so there is nothing to map here. Device-local memory that
        // is also host-visible — integrated GPUs, resizable BAR — arrives mapped too, which
        // skips the staging copy entirely.
        m_data->mapped = m_data->allocation.mapped;

        if (initialData) {
            // Seed every region so a dynamic buffer is valid on all frame slots from the start.
            for (uint32_t region = 0; region < m_data->regionCount; ++region) {
                update(initialData, size, 0, region);
            }
        }
    }

    VulkanBuffer::~VulkanBuffer()
    {
        VkDevice logicalDevice = m_data->device->logicalDevice();
        // Metal keeps a resource alive until every command buffer referencing it has completed.
        // Vulkan does not, so make sure no in-flight frame is still reading this memory.
        vkDeviceWaitIdle(logicalDevice);

        // The mapping belongs to the allocator's block, so it is not unmapped here.
        m_data->mapped = nullptr;
        if (m_data->buffer) vkDestroyBuffer(logicalDevice, m_data->buffer, nullptr);
        if (m_data->allocation.isValid()) m_data->device->allocator().free(m_data->allocation);
    }

    BufferType VulkanBuffer::type() const { return m_data->type; }
    BufferUsage VulkanBuffer::usage() const { return m_data->usage; }
    size_t VulkanBuffer::size() const { return m_data->size; }

    VkBufferUsageFlags VulkanBuffer::usageFlags() const { return m_data->usageFlags; }
    MemoryLocation VulkanBuffer::memoryLocation() const { return m_data->location; }

    void VulkanBuffer::update(const void* data, size_t dataSize, size_t offset)
    {
        if (m_data->regionCount <= 1) {
            update(data, dataSize, offset, 0);
            return;
        }
        update(data, dataSize, offset,
               m_data->device->currentFrameSlot() % m_data->regionCount);
    }

    void VulkanBuffer::update(const void* data, size_t dataSize, size_t offset, uint32_t region)
    {
        if (!data || dataSize == 0) return;
        if (offset + dataSize > m_data->size) {
            throw std::runtime_error("VulkanBuffer::update: write is out of bounds");
        }

        const VkDeviceSize destinationOffset = m_data->regionStride * region + offset;

        if (m_data->mapped) {
            char* base = static_cast<char*>(m_data->mapped);

            // A partial write leaves the rest of this region holding whatever it had two frames
            // ago, which is not what the caller means by "update these bytes". Bring the region
            // up to date from the newest copy first, so it becomes a complete snapshot that
            // happens to differ in the bytes being written now.
            //
            // Skipped when the write covers the whole buffer (nothing to carry) and when this
            // region already is the newest one (repeated updates within a frame).
            const bool coversWholeBuffer = (offset == 0 && dataSize == m_data->size);
            if (m_data->regionCount > 1 && !coversWholeBuffer && region != m_data->latestRegion) {
                // Reading the other region while the GPU may be reading it too is safe — this
                // only writes into the current frame's region, which the fence already guarded.
                std::memcpy(base + m_data->regionStride * region,
                            base + m_data->regionStride * m_data->latestRegion,
                            m_data->size);
            }

            std::memcpy(base + destinationOffset, data, dataSize);
            m_data->latestRegion = region;
            return;
        }

        // Device-local memory with no CPU mapping: the bytes have to travel through a staging
        // buffer and a GPU copy. This blocks until the copy completes, which is why the interface
        // documents update() as slow and points frequently-updated data at BufferUsage::Dynamic.
        //
        // No region carry-over is needed here. Only BufferUsage::Dynamic allocates more than one
        // region, and Dynamic always asks for host-visible memory, so an unmapped buffer is
        // always single-region and a partial write into it is already complete.
        m_data->device->uploadToDeviceLocalBuffer(m_data->buffer, destinationOffset, data, dataSize);
    }

    void VulkanBuffer::readback(void* destination, size_t destinationSize, size_t offset)
    {
        if (!destination || destinationSize == 0) return;
        if (offset + destinationSize > m_data->size) {
            throw std::runtime_error("VulkanBuffer::readback: read is out of bounds");
        }

        // Reads come from the region holding the newest contents, not blindly from region zero,
        // so a dynamic buffer reads back what the application last wrote.
        const VkDeviceSize sourceOffset = m_data->regionStride * m_data->latestRegion + offset;

        if (m_data->mapped) {
            std::memcpy(destination, static_cast<const char*>(m_data->mapped) + sourceOffset,
                        destinationSize);
            return;
        }

        m_data->device->readbackFromBuffer(m_data->buffer, sourceOffset, destination, destinationSize);
    }

    void* VulkanBuffer::nativeHandle() const
    {
        return (void*)&m_data->buffer;
    }

    const std::string& VulkanBuffer::debugName() const { return m_data->debugName; }

    void VulkanBuffer::setDebugName(const std::string& name)
    {
        m_data->debugName = name;
    }

    VkBuffer VulkanBuffer::buffer() const { return m_data->buffer; }

    VkDeviceSize VulkanBuffer::currentRegionOffset() const
    {
        if (m_data->regionCount <= 1) return 0;
        return m_data->regionStride * (m_data->device->currentFrameSlot() % m_data->regionCount);
    }

} // namespace dmrender
