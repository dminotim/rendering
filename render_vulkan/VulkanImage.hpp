//
// Created by Artem Avdoshkin on 14.08.2025.
//

#ifndef RENDERING_VULKANIMAGE_HPP
#define RENDERING_VULKANIMAGE_HPP

#include <vulkan/vulkan.h>

#include <memory>

#include "GImage.hpp"

namespace dmrender {

    class VulkanDevice;
    class VulkanSwapChain;
    struct VulkanImageNativeData;

    /**
     * @class VulkanImage
     * @brief GImage over either a swapchain image or a device-owned texture.
     *
     * The two cases differ only in ownership and in one piece of state:
     *
     *  - **Swapchain image.** Non-owning, exactly like MetalImage wrapping a CAMetalDrawable.
     *    The swapchain owns the VkImage and VkImageView. Its resting layout is PRESENT_SRC_KHR
     *    and `nativeDrawableHandle()` returns the owning swapchain, which is what
     *    `CommandBuffer::present()` needs.
     *
     *  - **Offscreen texture.** Owns a VkImage, its VkDeviceMemory and a VkImageView, created
     *    through `Device::createImage()`. Its resting layout follows from its usage, and
     *    `nativeDrawableHandle()` returns nullptr — it can never be presented.
     *
     * The resting layout is the whole of this backend's layout tracking. Every render pass leaves
     * its attachments in it and, when not clearing, expects to find them in it, so writing a
     * target in one pass and sampling it in the next needs no explicit barrier.
     */
    class VulkanImage : public GImage {
    public:
        /// @brief Constructs a non-owning view over a swapchain image.
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

        /// @brief Creates and owns an offscreen image, its memory and its view.
        VulkanImage(VulkanDevice* device,
                    ImageType type,
                    ImageFormat format,
                    uint32_t width,
                    uint32_t height,
                    ImageUsage usage,
                    const std::string& debugName);

        ~VulkanImage() override;

        uint32_t width() const override;
        uint32_t height() const override;
        uint32_t depth() const override;
        uint32_t mipLevels() const override;
        ImageFormat format() const override;
        ImageType type() const override;
        ImageUsage usage() const override;
        MemoryLocation memoryLocation() const override;

        /// @return Pointer to the VkImage handle.
        void* nativeHandle() const override;

        /**
         * @brief Vulkan's stand-in for a CAMetalDrawable.
         * @return The owning VulkanSwapChain for a swapchain image, nullptr for an offscreen one.
         */
        void* nativeDrawableHandle() const override;

        const std::string& debugName() const override;
        void setDebugName(const std::string& name) override;

        VkImageView imageView() const;

        /// @brief Valid only for swapchain images; 0 otherwise.
        uint32_t imageIndex() const;

        /// @brief The owning swapchain, or nullptr for an offscreen image.
        VulkanSwapChain* swapChain() const;

        /// @brief The layout this image sits in between render passes. See the class docs.
        VkImageLayout restingLayout() const;

    private:
        std::unique_ptr<VulkanImageNativeData> m_data;
    };

} // namespace dmrender

#endif //RENDERING_VULKANIMAGE_HPP
