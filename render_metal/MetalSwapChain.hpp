//
// Created by Artem Avdoshkin on 13.06.2025.
//

#ifndef RENDERING_METALSWAPCHAIN_HPP
#define RENDERING_METALSWAPCHAIN_HPP

#include "SwapChain.hpp"
#include "GImage.hpp"
#include "Device.hpp"

namespace dmrender {

    struct MetalSwapChainNativeData;

    class MetalSwapChain : public SwapChain {
    public:
        MetalSwapChain(std::shared_ptr<Device> device,
                       std::shared_ptr<CommandQueue> cmdLists, std::shared_ptr<Surface> outSurf, const size_t width, const size_t height);

        ~MetalSwapChain() override;

        std::shared_ptr<GImage>  acquireNextImage() override;

        std::uint32_t width() const override;
        std::uint32_t height() const override;
        void recreate(uint32_t width, uint32_t height) override;

    private:
        /// @brief Pointer to implementation (PIMPL) to hide Metal-specific details.
        std::unique_ptr<MetalSwapChainNativeData> m_data;
    };
}
#endif //RENDERING_METALSWAPCHAIN_HPP
