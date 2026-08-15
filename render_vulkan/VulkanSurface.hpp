//
// Created by Artem Avdoshkin on 12.07.2025.
//

#ifndef RENDERING_VULKANSURFACE_HPP
#define RENDERING_VULKANSURFACE_HPP

#include "Surface.hpp"

namespace dmrender {

    struct VulkanSurfaceNativeData;
    class VulkanSurface : public Surface {
    public:
        VulkanSurface(GLFWwindow* window, ImageFormat imageFormat_);

        ~VulkanSurface() override;

        void* nativeHandle() const override;

    private:
        /// @brief Pointer to implementation (PIMPL) to hide Vulkan-specific details.
        std::unique_ptr<VulkanSurfaceNativeData> m_data;
    };
}

#endif //RENDERING_VULKANSURFACE_HPP
