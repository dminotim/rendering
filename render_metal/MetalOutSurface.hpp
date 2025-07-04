//
// Created by Artem Avdoshkin on 13.06.2025.
//

#ifndef RENDERING_METALOUTSURFACE_HPP
#define RENDERING_METALOUTSURFACE_HPP

#include "Surface.hpp"

namespace dmrender {

struct MetalOutSurfaceNativeData;

class MetalOutSurface : public Surface {
public:
    MetalOutSurface(GLFWwindow *window, ImageFormat imageFormat_);

    ~MetalOutSurface() override;

    void *nativeHandle() const override;

private:
    /// @brief Pointer to implementation (PIMPL) to hide Metal-specific details.
    std::unique_ptr<MetalOutSurfaceNativeData> m_data;
};
}

#endif //RENDERING_METALOUTSURFACE_HPP
