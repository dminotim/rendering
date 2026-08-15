//
// Created by Artem Avdoshkin on 15.08.2025.
//

#ifndef RENDERING_METALSAMPLER_HPP
#define RENDERING_METALSAMPLER_HPP

#include <memory>

#include "GSampler.hpp"
#include "Device.hpp"

namespace dmrender {

    struct MetalSamplerNativeData;

    /**
     * @class MetalSampler
     * @brief An id<MTLSamplerState>, the direct counterpart of VkSampler.
     */
    class MetalSampler : public GSampler {
    public:
        MetalSampler(Device* device,
                     const SamplerDesc& desc,
                     const std::string& debugName);

        ~MetalSampler() override;

        void* nativeHandle() const override;

        const SamplerDesc& desc() const override;
        const std::string& debugName() const override;

    private:
        /// @brief Pointer to implementation (PIMPL) to hide Metal-specific details.
        std::unique_ptr<MetalSamplerNativeData> m_data;
        SamplerDesc m_desc;
        std::string m_debugName;
    };
}
#endif //RENDERING_METALSAMPLER_HPP
