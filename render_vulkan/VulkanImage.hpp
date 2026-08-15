//
// Created by Artem Avdoshkin on 14.08.2025.
//

#ifndef RENDERING_VULKANIMAGE_HPP
#define RENDERING_VULKANIMAGE_HPP

#include <vulkan/vulkan.h>

#include <memory>

#include "GImage.hpp"

namespace dmrender {

    class VulkanSwapChain;
    struct VulkanImageNativeData;

    /**
     * @class VulkanImage
     * @brief GImage view over one swapchain image.
     *
     * This is the counterpart of MetalImage wrapping a CAMetalDrawable: it does not own the
     * VkImage or the VkImageView, the swapchain does. It only carries enough context for the
     * render pass descriptor to find a framebuffer and for `CommandBuffer::present()` to know
     * which swapchain image to hand back to the presentation engine.
     */
    class VulkanImage : public GImage {
    public:
        VulkanImage(VulkanSwapChain* swapChain,
                    uint32_t imageIndex,
                    VkImage image,
                    VkImageView imageView,
                    ImageFormat format,
                    uint32_t width,
                    uint32_t height,
                    ImageUsage usage,
                    ImageType type,
                    const std::string& debugName = "");

        ~VulkanImage() override;

        uint32_t width() const override;
        uint32_t height() const override;
        uint32_t depth() const override;
        uint32_t mipLevels() const override;
        ImageFormat format() const override;
        ImageType type() const override;
        ImageUsage usage() const override;

        /// @return Pointer to the VkImage handle.
        void* nativeHandle() const override;

        /**
         * @brief Vulkan's stand-in for a CAMetalDrawable.
         * @return The owning VulkanSwapChain, which together with imageIndex() identifies
         *         exactly what `vkQueuePresentKHR` has to be told.
         */
        void* nativeDrawableHandle() const override;

        const std::string& debugName() const override;
        void setDebugName(const std::string& name) override;

        VkImageView imageView() const;
        uint32_t imageIndex() const;
        VulkanSwapChain* swapChain() const;

    private:
        std::unique_ptr<VulkanImageNativeData> m_data;
    };

} // namespace dmrender

#endif //RENDERING_VULKANIMAGE_HPP
