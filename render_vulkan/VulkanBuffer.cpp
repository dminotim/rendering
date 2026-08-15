#include "VulkanBuffer.hpp"

#include <cstring>
#include <stdexcept>

#include <render_vulkan/VulkanDevice.hpp>
#include <render_vulkan/VulkanUtils.hpp>

namespace dmrender {

    struct VulkanBufferNativeData
    {
        VulkanDevice* device = nullptr;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;

        BufferType type = BufferType::Vertex;
        BufferUsage usage = BufferUsage::Static;
        size_t size = 0;            ///< Logical size the caller asked for.
        VkDeviceSize regionStride = 0;  ///< Distance between two per-frame regions.
        uint32_t regionCount = 1;
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
                    return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            }
            return 0;
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
        if (type == BufferType::Uniform) alignment = limits.minUniformBufferOffsetAlignment;
        if (type == BufferType::Vertex)  alignment = limits.minStorageBufferOffsetAlignment;
        m_data->regionStride = alignUp(static_cast<VkDeviceSize>(size), alignment);

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = m_data->regionStride * m_data->regionCount;
        bufferInfo.usage = toBufferUsage(type);
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkDevice logicalDevice = device->logicalDevice();
        VkCheck(vkCreateBuffer(logicalDevice, &bufferInfo, nullptr, &m_data->buffer), "vkCreateBuffer");

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(logicalDevice, m_data->buffer, &requirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = FindMemoryType(
            device->physicalDevice(),
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        VkCheck(vkAllocateMemory(logicalDevice, &allocInfo, nullptr, &m_data->memory), "vkAllocateMemory");
        VkCheck(vkBindBufferMemory(logicalDevice, m_data->buffer, m_data->memory, 0), "vkBindBufferMemory");
        VkCheck(vkMapMemory(logicalDevice, m_data->memory, 0, VK_WHOLE_SIZE, 0, &m_data->mapped), "vkMapMemory");

        if (initialData) {
            // Seed every region so a dynamic buffer is valid on all frame slots from the start.
            for (uint32_t region = 0; region < m_data->regionCount; ++region) {
                std::memcpy(static_cast<char*>(m_data->mapped) + region * m_data->regionStride,
                            initialData,
                            size);
            }
        }
    }

    VulkanBuffer::~VulkanBuffer()
    {
        VkDevice logicalDevice = m_data->device->logicalDevice();
        // Metal keeps a resource alive until every command buffer referencing it has completed.
        // Vulkan does not, so make sure no in-flight frame is still reading this memory.
        vkDeviceWaitIdle(logicalDevice);

        if (m_data->mapped) {
            vkUnmapMemory(logicalDevice, m_data->memory);
            m_data->mapped = nullptr;
        }
        if (m_data->buffer) vkDestroyBuffer(logicalDevice, m_data->buffer, nullptr);
        if (m_data->memory) vkFreeMemory(logicalDevice, m_data->memory, nullptr);
    }

    BufferType VulkanBuffer::type() const { return m_data->type; }
    BufferUsage VulkanBuffer::usage() const { return m_data->usage; }
    size_t VulkanBuffer::size() const { return m_data->size; }

    void VulkanBuffer::update(const void* data, size_t dataSize, size_t offset)
    {
        if (!data || dataSize == 0) return;
        if (offset + dataSize > m_data->size) {
            throw std::runtime_error("VulkanBuffer::update: write is out of bounds");
        }
        char* dst = static_cast<char*>(m_data->mapped) + currentRegionOffset() + offset;
        std::memcpy(dst, data, dataSize);
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
