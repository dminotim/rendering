#include "VulkanSwapChain.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <vector>

#include <GLFW/glfw3.h>

#include <render_vulkan/VulkanCommandQueue.hpp>
#include <render_vulkan/VulkanDevice.hpp>
#include <render_vulkan/VulkanImage.hpp>
#include <render_vulkan/VulkanUtils.hpp>

#include "Surface.hpp"

namespace dmrender {

    struct VulkanSwapChainNativeData
    {
        VulkanDevice* device = nullptr;
        VulkanCommandQueues* queue = nullptr;
        VkSurfaceKHR surface = VK_NULL_HANDLE;

        VkSwapchainKHR swapChain = VK_NULL_HANDLE;
        VkFormat imageFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};

        std::vector<VkImage> images;
        std::vector<VkImageView> imageViews;

        /// One per frame slot: signalled by vkAcquireNextImageKHR, waited on by the submit.
        std::vector<VkSemaphore> imageAvailableSemaphores;
        /// One per swapchain image: signalled by the submit, waited on by the present.
        std::vector<VkSemaphore> renderFinishedSemaphores;
        /// Fence of the last frame that rendered into each image, or VK_NULL_HANDLE.
        std::vector<VkFence> imagesInFlight;

        VkRenderPass presentationRenderPass = VK_NULL_HANDLE;
        bool needsRecreate = false;
    };

    namespace {

        struct SurfaceSupport {
            VkSurfaceCapabilitiesKHR capabilities{};
            std::vector<VkSurfaceFormatKHR> formats;
            std::vector<VkPresentModeKHR> presentModes;
        };

        SurfaceSupport querySurfaceSupport(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface)
        {
            SurfaceSupport support;
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &support.capabilities);

            uint32_t formatCount = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
            support.formats.resize(formatCount);
            if (formatCount) {
                vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, support.formats.data());
            }

            uint32_t presentModeCount = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, nullptr);
            support.presentModes.resize(presentModeCount);
            if (presentModeCount) {
                vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount,
                                                          support.presentModes.data());
            }
            return support;
        }

        /**
         * @brief Picks the surface format that actually honours what Surface was created with.
         *
         * This matters for output parity with Metal: CAMetalLayer is set to BGRA8Unorm, a
         * *linear* format, so shader output lands in the framebuffer untouched. Picking the
         * sRGB variant here instead — a very common default — would silently re-encode every
         * colour and the same shader would produce a visibly different image.
         */
        VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available,
                                               ImageFormat requested)
        {
            const VkFormat wanted = ToVkFormat(requested);
            for (const auto& format : available) {
                if (format.format == wanted && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    return format;
                }
            }
            for (const auto& format : available) {
                if (format.format == wanted) return format;
            }
            return available.front();
        }

        /// FIFO matches CAMetalLayer's default vsync-locked presentation and is always available.
        VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& /*available*/)
        {
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities,
                                uint32_t width,
                                uint32_t height)
        {
            if (capabilities.currentExtent.width != UINT32_MAX) {
                return capabilities.currentExtent;
            }
            VkExtent2D extent{ width, height };
            extent.width = std::clamp(extent.width,
                                      capabilities.minImageExtent.width,
                                      capabilities.maxImageExtent.width);
            extent.height = std::clamp(extent.height,
                                       capabilities.minImageExtent.height,
                                       capabilities.maxImageExtent.height);
            return extent;
        }

    } // namespace

    VulkanSwapChain::VulkanSwapChain(const std::shared_ptr<Device>& device,
                                     const std::shared_ptr<CommandQueue>& commandQueue,
                                     const std::shared_ptr<Surface>& surface,
                                     uint32_t width,
                                     uint32_t height)
        : SwapChain(device, commandQueue, surface, width, height),
          m_data(std::make_unique<VulkanSwapChainNativeData>())
    {
        m_data->device = static_cast<VulkanDevice*>(m_device.get());
        m_data->queue = static_cast<VulkanCommandQueues*>(m_commandQueue.get());
        m_data->surface = *static_cast<VkSurfaceKHR*>(m_surface->nativeHandle());

        VkDevice logicalDevice = m_data->device->logicalDevice();

        m_data->imageAvailableSemaphores.resize(kFramesInFlight, VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkCheck(vkCreateSemaphore(logicalDevice, &semaphoreInfo, nullptr, &m_data->imageAvailableSemaphores[i]),
                    "vkCreateSemaphore");
        }

        createSwapChainResources(width, height);
    }

    VulkanSwapChain::~VulkanSwapChain()
    {
        VkDevice logicalDevice = m_data->device->logicalDevice();
        vkDeviceWaitIdle(logicalDevice);

        destroySwapChainResources();

        for (VkSemaphore semaphore : m_data->imageAvailableSemaphores) {
            if (semaphore) vkDestroySemaphore(logicalDevice, semaphore, nullptr);
        }
        m_data->imageAvailableSemaphores.clear();
    }

    void VulkanSwapChain::createSwapChainResources(uint32_t width, uint32_t height)
    {
        VkPhysicalDevice physicalDevice = m_data->device->physicalDevice();
        VkDevice logicalDevice = m_data->device->logicalDevice();

        SurfaceSupport support = querySurfaceSupport(physicalDevice, m_data->surface);
        if (support.formats.empty() || support.presentModes.empty()) {
            throw std::runtime_error("VulkanSwapChain: surface does not support any format or present mode");
        }

        const VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(support.formats, m_surface->getFormat());
        const VkPresentModeKHR presentMode = choosePresentMode(support.presentModes);
        const VkExtent2D extent = chooseExtent(support.capabilities, width, height);

        uint32_t imageCount = support.capabilities.minImageCount + 1;
        if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount) {
            imageCount = support.capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = m_data->surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        const uint32_t queueFamilyIndices[] = {
            m_data->device->getGraphicsFamilyIndex(),
            m_data->device->getPresentFamilyIndex()
        };
        if (queueFamilyIndices[0] != queueFamilyIndices[1]) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        createInfo.preTransform = support.capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        VkCheck(vkCreateSwapchainKHR(logicalDevice, &createInfo, nullptr, &m_data->swapChain),
                "vkCreateSwapchainKHR");

        m_data->imageFormat = surfaceFormat.format;
        m_data->extent = extent;
        m_width = extent.width;
        m_height = extent.height;

        uint32_t actualImageCount = 0;
        vkGetSwapchainImagesKHR(logicalDevice, m_data->swapChain, &actualImageCount, nullptr);
        m_data->images.resize(actualImageCount);
        vkGetSwapchainImagesKHR(logicalDevice, m_data->swapChain, &actualImageCount, m_data->images.data());

        m_data->imageViews.resize(actualImageCount, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < actualImageCount; ++i) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = m_data->images[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = m_data->imageFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;
            VkCheck(vkCreateImageView(logicalDevice, &viewInfo, nullptr, &m_data->imageViews[i]),
                    "vkCreateImageView");
        }

        m_data->renderFinishedSemaphores.resize(actualImageCount, VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (uint32_t i = 0; i < actualImageCount; ++i) {
            VkCheck(vkCreateSemaphore(logicalDevice, &semaphoreInfo, nullptr, &m_data->renderFinishedSemaphores[i]),
                    "vkCreateSemaphore");
        }

        m_data->imagesInFlight.assign(actualImageCount, VK_NULL_HANDLE);

        // ImGui builds its pipeline against this one at init time. It only needs compatibility —
        // one colour attachment of this format — so the clear variant serves either way.
        RenderPassKey key{};
        key.colors.push_back(RenderPassAttachmentKey{
            m_data->imageFormat, /*clear=*/true, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR });
        m_data->presentationRenderPass = m_data->device->acquireRenderPass(key);

        m_data->needsRecreate = false;
    }

    void VulkanSwapChain::destroySwapChainResources()
    {
        VkDevice logicalDevice = m_data->device->logicalDevice();

        for (VkSemaphore semaphore : m_data->renderFinishedSemaphores) {
            if (semaphore) vkDestroySemaphore(logicalDevice, semaphore, nullptr);
        }
        m_data->renderFinishedSemaphores.clear();

        for (VkImageView view : m_data->imageViews) {
            if (!view) continue;
            // The framebuffer cache lives on the device now, so it has to be told before these
            // views stop being valid.
            m_data->device->invalidateFramebuffersUsing(view);
            vkDestroyImageView(logicalDevice, view, nullptr);
        }
        m_data->imageViews.clear();
        m_data->images.clear();
        m_data->imagesInFlight.clear();

        if (m_data->swapChain) {
            vkDestroySwapchainKHR(logicalDevice, m_data->swapChain, nullptr);
            m_data->swapChain = VK_NULL_HANDLE;
        }
    }

    std::shared_ptr<GImage> VulkanSwapChain::acquireNextImage()
    {
        // Opening the frame first is what makes the acquire safe: it blocks until the frame slot
        // (command buffer, descriptor pool and the semaphore below) is no longer in use.
        m_data->queue->beginFrame();

        if (m_data->needsRecreate) {
            recreate(m_width, m_height);
            return nullptr;
        }

        VkDevice logicalDevice = m_data->device->logicalDevice();
        const VkSemaphore imageAvailable = currentImageAvailableSemaphore();

        uint32_t imageIndex = 0;
        const VkResult result = vkAcquireNextImageKHR(logicalDevice,
                                                      m_data->swapChain,
                                                      UINT64_MAX,
                                                      imageAvailable,
                                                      VK_NULL_HANDLE,
                                                      &imageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            // The semaphore is untouched when the acquire fails this way, so the frame slot can
            // be reused as-is on the next iteration.
            recreate(m_width, m_height);
            return nullptr;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            VkCheck(result, "vkAcquireNextImageKHR");
        }

        // Do not start recording into an image a previous frame is still rendering to.
        if (m_data->imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
            VkCheck(vkWaitForFences(logicalDevice, 1, &m_data->imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX),
                    "vkWaitForFences");
        }
        m_data->imagesInFlight[imageIndex] = m_data->queue->currentFence();

        return std::make_shared<VulkanImage>(this,
                                             imageIndex,
                                             m_data->images[imageIndex],
                                             m_data->imageViews[imageIndex],
                                             FromVkFormat(m_data->imageFormat),
                                             m_data->extent.width,
                                             m_data->extent.height,
                                             ImageUsage::ColorTarget,
                                             ImageType::Image2D,
                                             "SwapChainImage");
    }

    uint32_t VulkanSwapChain::width() const { return m_width; }
    uint32_t VulkanSwapChain::height() const { return m_height; }

    void VulkanSwapChain::recreate(uint32_t newWidth, uint32_t newHeight)
    {
        // A minimised window reports a zero-sized framebuffer, which Vulkan refuses to swap on.
        // Block here in exactly the same way the raw Vulkan sample did.
        if (GLFWwindow* window = m_surface->getWindow()) {
            int width = static_cast<int>(newWidth);
            int height = static_cast<int>(newHeight);
            glfwGetFramebufferSize(window, &width, &height);
            while (width == 0 || height == 0) {
                glfwGetFramebufferSize(window, &width, &height);
                glfwWaitEvents();
            }
            newWidth = static_cast<uint32_t>(width);
            newHeight = static_cast<uint32_t>(height);
        }

        vkDeviceWaitIdle(m_data->device->logicalDevice());
        destroySwapChainResources();
        createSwapChainResources(newWidth, newHeight);
    }

    void VulkanSwapChain::present(uint32_t imageIndex, VkSemaphore waitSemaphore)
    {
        VkSwapchainKHR swapChain = m_data->swapChain;

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &waitSemaphore;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapChain;
        presentInfo.pImageIndices = &imageIndex;

        const VkResult result = vkQueuePresentKHR(m_data->queue->presentQueue(), &presentInfo);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            // Rebuilding right here would destroy objects the just-submitted frame still uses,
            // so only flag it; the next acquireNextImage() does the work.
            m_data->needsRecreate = true;
        } else if (result != VK_SUCCESS) {
            VkCheck(result, "vkQueuePresentKHR");
        }
    }

    VkSemaphore VulkanSwapChain::currentImageAvailableSemaphore() const
    {
        return m_data->imageAvailableSemaphores[m_data->queue->currentFrameSlot()];
    }

    VkSemaphore VulkanSwapChain::renderFinishedSemaphore(uint32_t imageIndex) const
    {
        return m_data->renderFinishedSemaphores[imageIndex];
    }

    VkFormat VulkanSwapChain::imageFormat() const { return m_data->imageFormat; }
    VkExtent2D VulkanSwapChain::extent() const { return m_data->extent; }
    uint32_t VulkanSwapChain::imageCount() const { return static_cast<uint32_t>(m_data->images.size()); }
    VkRenderPass VulkanSwapChain::presentationRenderPass() const { return m_data->presentationRenderPass; }

} // namespace dmrender
