//
// Created by Artem Avdoshkin on 15.08.2025.
//

#ifndef RENDERING_VULKANSAMPLER_HPP
#define RENDERING_VULKANSAMPLER_HPP

#include <vulkan/vulkan.h>

#include <memory>

#include "GSampler.hpp"

namespace dmrender {

    class VulkanDevice;
    struct VulkanSamplerNativeData;

    /**
     * @class VulkanSampler
     * @brief A VkSampler, the direct counterpart of id<MTLSamplerState>.
     */
    class VulkanSampler : public GSampler {
    public:
        VulkanSampler(VulkanDevice* device, const SamplerDesc& desc, const std::string& debugName);

        ~VulkanSampler() override;

        /// @return Pointer to the VkSampler handle.
        void* nativeHandle() const override;

        const SamplerDesc& desc() const override;
        const std::string& debugName() const override;

        VkSampler sampler() const;

    private:
        std::unique_ptr<VulkanSamplerNativeData> m_data;
    };

} // namespace dmrender

#endif //RENDERING_VULKANSAMPLER_HPP
