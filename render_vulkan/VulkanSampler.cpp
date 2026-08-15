#include "VulkanSampler.hpp"

#include <render_vulkan/VulkanDevice.hpp>
#include <render_vulkan/VulkanUtils.hpp>

namespace dmrender {

    struct VulkanSamplerNativeData
    {
        VulkanDevice* device = nullptr;
        VkSampler sampler = VK_NULL_HANDLE;
        SamplerDesc desc;
        std::string debugName;
    };

    VulkanSampler::VulkanSampler(VulkanDevice* device, const SamplerDesc& desc, const std::string& debugName)
        : m_data(std::make_unique<VulkanSamplerNativeData>())
    {
        m_data->device = device;
        m_data->desc = desc;
        m_data->debugName = debugName;

        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = ToVkFilter(desc.magFilter);
        info.minFilter = ToVkFilter(desc.minFilter);
        info.addressModeU = ToVkAddressMode(desc.addressU);
        info.addressModeV = ToVkAddressMode(desc.addressV);
        info.addressModeW = ToVkAddressMode(desc.addressW);
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.minLod = 0.0f;
        info.maxLod = 0.0f;
        // Anisotropy is a device feature that has to be enabled at device creation; the
        // abstraction does not expose it, so it stays off on both backends.
        info.anisotropyEnable = VK_FALSE;
        info.maxAnisotropy = 1.0f;
        info.compareEnable = VK_FALSE;
        info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        info.unnormalizedCoordinates = VK_FALSE;

        VkCheck(vkCreateSampler(device->logicalDevice(), &info, nullptr, &m_data->sampler),
                "vkCreateSampler");
    }

    VulkanSampler::~VulkanSampler()
    {
        if (m_data->sampler) {
            VkDevice logicalDevice = m_data->device->logicalDevice();
            vkDeviceWaitIdle(logicalDevice);
            vkDestroySampler(logicalDevice, m_data->sampler, nullptr);
            m_data->sampler = VK_NULL_HANDLE;
        }
    }

    void* VulkanSampler::nativeHandle() const { return (void*)&m_data->sampler; }

    const SamplerDesc& VulkanSampler::desc() const { return m_data->desc; }

    const std::string& VulkanSampler::debugName() const { return m_data->debugName; }

    VkSampler VulkanSampler::sampler() const { return m_data->sampler; }

} // namespace dmrender
