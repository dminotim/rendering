//
// Created by Artem Avdoshkin on 14.08.2025.
//

#ifndef RENDERING_VULKANSWAPCHAIN_HPP
#define RENDERING_VULKANSWAPCHAIN_HPP

#include <vulkan/vulkan.h>

#include <memory>

#include "SwapChain.hpp"
#include "GImage.hpp"
#include "Device.hpp"

namespace dmrender {

    class VulkanCommandQueues;
    struct VulkanSwapChainNativeData;

    /**
     * @class VulkanSwapChain
     * @brief VkSwapchainKHR wrapped behind the same interface CAMetalLayer is wrapped in.
     *
     * On Metal, `acquireNextImage()` is a single call to `[layer nextDrawable]` and the driver
     * hides all the pacing. Vulkan needs that pacing spelled out, so this class also owns the
     * synchronisation primitives that belong to presentation:
     *
     *  - one "image available" semaphore per frame slot, signalled by vkAcquireNextImageKHR,
     *  - one "render finished" semaphore per swapchain image, signalled by the submit and waited
     *    on by vkQueuePresentKHR (per image, not per frame, so a semaphore is never re-used while
     *    a present that waits on it is still pending),
     *  - a note of which frame fence last touched each image, so an image is never recorded into
     *    while a previous frame is still reading it.
     *
     * The per-frame CPU/GPU fence itself lives in VulkanCommandQueues, because it also gates
     * command buffer and descriptor pool reuse. `acquireNextImage()` asks the queue to open the
     * frame before touching the swapchain.
     *
     * Framebuffers are cached on the device, since a render pass may target offscreen images
     * only. This class is still responsible for calling `invalidateFramebuffersUsing()` for each
     * of its image views before destroying them in `recreate()`.
     */
    class VulkanSwapChain : public SwapChain {
    public:
        VulkanSwapChain(const std::shared_ptr<Device>& device,
                        const std::shared_ptr<CommandQueue>& commandQueue,
                        const std::shared_ptr<Surface>& surface,
                        uint32_t width,
                        uint32_t height);

        ~VulkanSwapChain() override;

        /**
         * @brief Opens the frame and acquires the next presentable image.
         * @return nullptr when the swapchain had to be rebuilt (window resized or minimised);
         *         the caller should skip the frame, exactly as on Metal when there is no drawable.
         */
        std::shared_ptr<GImage> acquireNextImage() override;

        uint32_t width() const override;
        uint32_t height() const override;

        void recreate(uint32_t newWidth, uint32_t newHeight) override;

        // --- Used by the rest of the Vulkan backend ---

        /// @brief Presents the image acquired this frame; called from VulkanCommandBuffer::commit().
        void present(uint32_t imageIndex, VkSemaphore waitSemaphore);

        /// @brief Semaphore vkAcquireNextImageKHR signalled for the frame being recorded.
        VkSemaphore currentImageAvailableSemaphore() const;

        /// @brief Semaphore the submit must signal so that presenting @p imageIndex can wait on it.
        VkSemaphore renderFinishedSemaphore(uint32_t imageIndex) const;

        VkFormat imageFormat() const;
        VkExtent2D extent() const;
        uint32_t imageCount() const;

        /// @brief A render pass matching this swapchain's colour format, handed to ImGui.
        VkRenderPass presentationRenderPass() const;

    private:
        void createSwapChainResources(uint32_t width, uint32_t height);
        void destroySwapChainResources();

        std::unique_ptr<VulkanSwapChainNativeData> m_data;
    };

} // namespace dmrender

#endif //RENDERING_VULKANSWAPCHAIN_HPP
