#include "VulkanUtils.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

#include "Commandbuffer.hpp"
#include "RenderPassDescriptor.hpp"

#include <render_vulkan/VulkanCommandQueue.hpp>
#include <render_vulkan/VulkanDevice.hpp>
#include <render_vulkan/VulkanSingleton.hpp>
#include <render_vulkan/VulkanSwapChain.hpp>

namespace dmrender {

    void VkCheck(VkResult result, const char* what)
    {
        if (result != VK_SUCCESS) {
            throw std::runtime_error(std::string(what) + " failed with VkResult " + std::to_string(result));
        }
    }

    VkFormat ToVkFormat(ImageFormat format)
    {
        switch (format) {
            case ImageFormat::RGBA8_UNORM:       return VK_FORMAT_R8G8B8A8_UNORM;
            case ImageFormat::BGRA8_UNORM:       return VK_FORMAT_B8G8R8A8_UNORM;
            case ImageFormat::RGBA16_FLOAT:      return VK_FORMAT_R16G16B16A16_SFLOAT;
            case ImageFormat::R32_FLOAT:         return VK_FORMAT_R32_SFLOAT;

            case ImageFormat::D32_FLOAT:         return VK_FORMAT_D32_SFLOAT;
            case ImageFormat::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
            case ImageFormat::D16_UNORM:         return VK_FORMAT_D16_UNORM;

            case ImageFormat::Undefined:
            default:                             return VK_FORMAT_UNDEFINED;
        }
    }

    ImageFormat FromVkFormat(VkFormat format)
    {
        switch (format) {
            case VK_FORMAT_R8G8B8A8_UNORM:      return ImageFormat::RGBA8_UNORM;
            case VK_FORMAT_B8G8R8A8_UNORM:      return ImageFormat::BGRA8_UNORM;
            case VK_FORMAT_R16G16B16A16_SFLOAT: return ImageFormat::RGBA16_FLOAT;
            case VK_FORMAT_R32_SFLOAT:          return ImageFormat::R32_FLOAT;

            case VK_FORMAT_D32_SFLOAT:          return ImageFormat::D32_FLOAT;
            case VK_FORMAT_D24_UNORM_S8_UINT:   return ImageFormat::D24_UNORM_S8_UINT;
            case VK_FORMAT_D16_UNORM:           return ImageFormat::D16_UNORM;

            default:                            return ImageFormat::Undefined;
        }
    }

    bool IsDepthFormat(ImageFormat format)
    {
        switch (format) {
            case ImageFormat::D32_FLOAT:
            case ImageFormat::D24_UNORM_S8_UINT:
            case ImageFormat::D16_UNORM:
                return true;
            default:
                return false;
        }
    }

    VkFilter ToVkFilter(SamplerFilter filter)
    {
        return (filter == SamplerFilter::Nearest) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    }

    VkSamplerAddressMode ToVkAddressMode(SamplerAddressMode mode)
    {
        switch (mode) {
            case SamplerAddressMode::Repeat:         return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case SamplerAddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case SamplerAddressMode::ClampToEdge:
            default:                                 return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        }
    }

    VkImageUsageFlags ToVkImageUsage(ImageUsage usage)
    {
        VkImageUsageFlags flags = 0;
        if (hasFlag(usage, ImageUsage::Sampled))      flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (hasFlag(usage, ImageUsage::Storage))      flags |= VK_IMAGE_USAGE_STORAGE_BIT;
        if (hasFlag(usage, ImageUsage::ColorTarget))  flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (hasFlag(usage, ImageUsage::DepthStencil)) flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (hasFlag(usage, ImageUsage::TransferSrc))  flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (hasFlag(usage, ImageUsage::TransferDst))  flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        return flags;
    }

    VkImageLayout RestingLayoutFor(ImageUsage usage, bool isSwapChainImage)
    {
        // A swapchain image always goes back to the presentation engine.
        if (isSwapChainImage) return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        if (hasFlag(usage, ImageUsage::DepthStencil)) {
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }
        // A colour target that will also be sampled rests in the layout the sampler needs, so a
        // render pass writing it and a later pass reading it need no barrier between them — the
        // render pass's own final layout and external dependency do the work.
        if (hasFlag(usage, ImageUsage::Sampled)) {
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    uint32_t FindMemoryType(VkPhysicalDevice physicalDevice,
                            uint32_t typeFilter,
                            VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            const bool typeAllowed = (typeFilter & (1u << i)) != 0;
            const bool hasProperties =
                (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
            if (typeAllowed && hasProperties) {
                return i;
            }
        }
        throw std::runtime_error("FindMemoryType: no memory type satisfies the requested properties");
    }

    // ------------------------------------------------------------------------
    // ImGui glue
    // ------------------------------------------------------------------------
    //
    // ImGui_ImplVulkan needs far more context than ImGui_ImplMetal: a queue, a descriptor pool of
    // its own and a render pass to build its pipeline against. That is why initImgui() takes the
    // swapchain rather than just the device — by then every one of those exists.
    //
    // The state below is file-static for the same reason the Metal backend keeps none: the ImGui
    // backend itself is a singleton, so there can only ever be one of these.

    namespace {
        VkDevice g_imguiDevice = VK_NULL_HANDLE;
        VkDescriptorPool g_imguiDescriptorPool = VK_NULL_HANDLE;

        void imguiCheckVkResult(VkResult err)
        {
            if (err != VK_SUCCESS) {
                std::cerr << "ImGui Vulkan error: VkResult " << err << std::endl;
            }
        }
    }

    bool InitImguiVulkan(const std::shared_ptr<SwapChain>& swapChain)
    {
        if (!swapChain) return false;

        auto* vulkanSwapChain = static_cast<VulkanSwapChain*>(swapChain.get());
        auto* device = static_cast<VulkanDevice*>(vulkanSwapChain->getDevice().get());
        auto* queue = static_cast<VulkanCommandQueues*>(vulkanSwapChain->getCommandQueue().get());

        VkDevice logicalDevice = device->logicalDevice();

        const VkDescriptorPoolSize poolSizes[] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 },
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        // ImGui frees individual sets when a texture is destroyed, so the pool must allow it.
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 64;
        poolInfo.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes));
        poolInfo.pPoolSizes = poolSizes;
        VkCheck(vkCreateDescriptorPool(logicalDevice, &poolInfo, nullptr, &g_imguiDescriptorPool),
                "vkCreateDescriptorPool (ImGui)");
        g_imguiDevice = logicalDevice;

        ImGui_ImplVulkan_InitInfo initInfo{};
        // Must match what VulkanSingleton asked for, otherwise the backend may try to use entry
        // points the instance was never created with.
        initInfo.ApiVersion = VK_API_VERSION_1_2;
        initInfo.Instance = VulkanSingleton::getInstance().nativeHandle();
        initInfo.PhysicalDevice = device->physicalDevice();
        initInfo.Device = logicalDevice;
        initInfo.QueueFamily = device->getGraphicsFamilyIndex();
        initInfo.Queue = queue->graphicsQueue();
        initInfo.DescriptorPool = g_imguiDescriptorPool;
        initInfo.RenderPass = vulkanSwapChain->presentationRenderPass();
        initInfo.MinImageCount = vulkanSwapChain->imageCount();
        initInfo.ImageCount = vulkanSwapChain->imageCount();
        initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        initInfo.CheckVkResultFn = imguiCheckVkResult;

        // The font atlas is uploaded lazily by ImGui_ImplVulkan_NewFrame(), so there is no
        // one-off command buffer submission to do here.
        return ImGui_ImplVulkan_Init(&initInfo);
    }

    bool NewFrameImguiVulkan(const std::shared_ptr<RenderPassDescriptor>& /*passDesc*/)
    {
        // The Metal backend needs the pass descriptor to know its target's pixel format; the
        // Vulkan backend was told at init time and ignores it.
        ImGui_ImplVulkan_NewFrame();
        return true;
    }

    bool RenderInternalImguiVulkan(const std::shared_ptr<CommandBuffer>& cmdBuffer)
    {
        if (!cmdBuffer) return false;
        auto commandBuffer = static_cast<VkCommandBuffer>(cmdBuffer->nativeHandle());
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
        return true;
    }

    bool ShutdownImguiVulkan()
    {
        // ImGui_ImplVulkan_Shutdown() destroys its own pipeline, geometry buffers and font
        // descriptor set outright. The last frame is still pending on the GPU at this point, so
        // without this wait every one of those destroys trips VUID-vkDestroyBuffer-buffer-00922
        // and friends. Metal needs no equivalent: it keeps resources alive until the command
        // buffers referencing them retire.
        if (g_imguiDevice) {
            vkDeviceWaitIdle(g_imguiDevice);
        }

        ImGui_ImplVulkan_Shutdown();
        if (g_imguiDescriptorPool && g_imguiDevice) {
            vkDestroyDescriptorPool(g_imguiDevice, g_imguiDescriptorPool, nullptr);
            g_imguiDescriptorPool = VK_NULL_HANDLE;
            g_imguiDevice = VK_NULL_HANDLE;
        }
        return true;
    }

} // namespace dmrender
