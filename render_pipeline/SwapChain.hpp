//
// Created by Artem Avdoshkin on 11.06.2025.
//

#ifndef RENDERING_SWAPCHAIN_HPP
#define RENDERING_SWAPCHAIN_HPP

#include <memory>
#include <cstdint>

// Forward declarations for clarity and reduced include overhead
namespace dmrender {
    class Device;
    class CommandQueue;
    class Surface;
    class GImage;
}

namespace dmrender
{
    /**
     * @class SwapChain
     * @brief Manages a collection of framebuffers (images) for presentation to a surface.
     *
     * A SwapChain is essential for displaying rendered frames without tearing. It typically
     * consists of a queue of images (e.g., double or triple buffering) that are cycled through:
     * one is displayed while another is being rendered to.
     */
    class SwapChain
    {
    public:
        /**
         * @brief Constructs a SwapChain.
         * @param device The logical device.
         * @param commandQueue The command queue used for presentation.
         * @param surface The target surface to present images to.
         * @param width The initial width of the swapchain images.
         * @param height The initial height of the swapchain images.
         */
        explicit SwapChain(const std::shared_ptr<Device>& device,
                           const std::shared_ptr<CommandQueue>& commandQueue,
                           const std::shared_ptr<Surface>& surface,
                           uint32_t width,
                           uint32_t height)
                : m_device(device),
                  m_commandQueue(commandQueue),
                  m_surface(surface),
                  m_width(width),
                  m_height(height)
        {
        }

        virtual ~SwapChain() = default;

        // Prohibit copy and move operations as SwapChain is a unique resource.
        SwapChain(const SwapChain&) = delete;
        SwapChain& operator=(const SwapChain&) = delete;
        SwapChain(SwapChain&&) = delete;
        SwapChain& operator=(SwapChain&&) = delete;

        /**
         * @brief Acquires the next available image from the swapchain to be rendered to.
         * @return A shared pointer to a GImage that can be used as a render target.
         *         Returns nullptr if the swapchain is out of date or unavailable (e.g., window minimized).
         */
        virtual std::shared_ptr<GImage> acquireNextImage() = 0;

        /**
         * @brief Gets the current width of the swapchain images.
         * @return The width in pixels.
         */
        virtual uint32_t width() const = 0;

        /**
         * @brief Gets the current height of the swapchain images.
         * @return The height in pixels.
         */
        virtual uint32_t height() const = 0;

        /**
         * @brief Recreates the swapchain with a new size.
         *
         * This must be called when the window is resized to ensure the swapchain images
         * match the new surface dimensions.
         *
         * @param newWidth The new width in pixels.
         * @param newHeight The new height in pixels.
         */
        virtual void recreate(uint32_t newWidth, uint32_t newHeight) = 0;

    protected:
        std::shared_ptr<Device> m_device;
        std::shared_ptr<CommandQueue> m_commandQueue;
        std::shared_ptr<Surface> m_surface;
        uint32_t m_width;
        uint32_t m_height;
    };

} // namespace dmrender
#endif //RENDERING_SWAPCHAIN_HPP
