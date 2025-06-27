//
// Created by Artem Avdoshkin on 15.06.2025.
//

#ifndef RENDERING_METALSHADERFUNCTION_HPP
#define RENDERING_METALSHADERFUNCTION_HPP
#include <memory>
#include "ShaderFunction.hpp"
#include "Device.hpp"

namespace dmrender {
struct MetalShaderFunctionNativeData;

class MetalShaderFunction : public ShaderFunction {
    public:
        MetalShaderFunction(const std::shared_ptr<Device>& device,
                            const std::string& source,
                            const std::string& entryPoint,
                            const std::string& label = "");

        ~MetalShaderFunction() override;

        void* nativeHandle() const override;
        const char* entryPoint() const override;

    private:
    /// @brief Pointer to implementation (PIMPL) to hide Metal-specific details.
        std::unique_ptr<MetalShaderFunctionNativeData> m_data;
        std::string m_entryPoint;
    };

}
#endif //RENDERING_METALSHADERFUNCTION_HPP
