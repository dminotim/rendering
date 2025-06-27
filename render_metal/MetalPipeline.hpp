//
// Created by Artem Avdoshkin on 15.06.2025.
//

#ifndef RENDERING_METALPIPELINE_HPP
#define RENDERING_METALPIPELINE_HPP
#include <memory>
#include "Device.hpp"
#include "ShaderFunction.hpp"
#include "Pipeline.hpp"
namespace dmrender {
    struct MetalPipelineNativeData;
    class MetalPipeline : public Pipeline {

    public:
        MetalPipeline(const std::shared_ptr<Device>& device,
                      const std::shared_ptr<ShaderFunction>& vertexFunction,
                      const std::shared_ptr<ShaderFunction>& fragmentFunction,
                      const RenderTargetFormat& targetFormat);

        ~MetalPipeline() override;

        void *nativeHandle() const override;
        const std::string& debugName() const override;
    private:
        /// @brief Pointer to implementation (PIMPL) to hide Metal-specific details.
        std::unique_ptr<MetalPipelineNativeData> m_data;
        std::string m_debugName;
    };
}
#endif //RENDERING_METALPIPELINE_HPP
