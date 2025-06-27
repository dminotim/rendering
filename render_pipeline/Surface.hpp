//
// Created by Artem Avdoshkin on 11.06.2025.
//

#ifndef RENDERING_SURFACE_HPP
#define RENDERING_SURFACE_HPP

#include <memory>
#include "GImage.hpp" // For ImageFormat

// Forward declarations to reduce include dependencies.
struct GLFWwindow;
namespace dmrender {
    class Device;
}

namespace dmrender {

    /**
     * @class Surface
     * @brief An abstract interface for a native window surface used for rendering.
     *
     * A Surface represents the platform-specific window or view that graphics will be
     * presented to. It is typically created from a window handle (e.g., from GLFW)
     * and a graphics device.
     */
    class Surface {
    public:
        virtual ~Surface() = default;

        // Prohibit copy and move operations.
        Surface(const Surface&) = delete;
        Surface& operator=(const Surface&) = delete;
        Surface(Surface&&) = delete;
        Surface& operator=(Surface&&) = delete;

        /**
         * @brief Retrieves the native, backend-specific handle for the surface.
         * @return A void pointer to the native object (e.g., NSView* on macOS, VkSurfaceKHR on Vulkan).
         */
        virtual void* nativeHandle() const = 0;

        /**
         * @brief Gets the pixel format requested for this surface.
         * @return The ImageFormat of the surface.
         */
        ImageFormat getFormat() const {
            return m_format;
        }

        /**
         * @brief Gets the graphics device associated with this surface.
         * @return A shared pointer to the Device.
         */
        std::shared_ptr<Device> getDevice() const {
            return m_device;
        }

        /**
         * @brief Gets the GLFW window handle this surface was created from.
         * @return A pointer to the GLFWwindow.
         */
        GLFWwindow* getWindow() const {
            return m_window;
        }

    protected:
        /**
         * @brief Protected constructor for derived classes.
         * @param window The GLFW window to create the surface from.
         * @param device The graphics device that will render to this surface.
         * @param format The desired pixel format for the surface.
         */
        explicit Surface(GLFWwindow* window, const std::shared_ptr<Device>& device, ImageFormat format)
                : m_window(window), m_device(device), m_format(format)
        {
        }

        /// @brief The GLFW window associated with this surface.
        GLFWwindow* m_window;
        /// @brief The graphics device that owns this surface.
        std::shared_ptr<Device> m_device;
        /// @brief The pixel format of the surface.
        ImageFormat m_format;
    };

} // namespace dmrender

#endif //RENDERING_SURFACE_HPP
