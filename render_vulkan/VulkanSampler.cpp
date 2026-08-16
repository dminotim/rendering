#include "VulkanSampler.hpp"

#include <algorithm>

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
        info.mipmapMode = (desc.mipFilter == SamplerFilter::Nearest)
            ? VK_SAMPLER_MIPMAP_MODE_NEAREST
            : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.minLod = desc.minLod;
        info.maxLod = desc.maxLod;

        // Clamp rather than reject: a caller asking for 16 on hardware that offers 8 wants the
        // best available, not an error. maxSupportedAnisotropy() reports 1 when the device feature
        // could not be enabled, which switches this off entirely.
        const uint32_t supported = device->maxSupportedAnisotropy();
        const uint32_t requested = std::min(std::max(desc.maxAnisotropy, 1u), supported);
        info.anisotropyEnable = requested > 1 ? VK_TRUE : VK_FALSE;
        info.maxAnisotropy = static_cast<float>(requested);

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
