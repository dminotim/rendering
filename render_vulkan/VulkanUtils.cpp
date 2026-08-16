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

            // BC is exposed on Vulkan as the BC_* family, gated by the textureCompressionBC
            // device feature which every desktop GPU supports.
            case ImageFormat::BC1_RGBA_UNORM:    return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            case ImageFormat::BC1_RGBA_SRGB:     return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
            case ImageFormat::BC3_UNORM:         return VK_FORMAT_BC3_UNORM_BLOCK;
            case ImageFormat::BC3_SRGB:          return VK_FORMAT_BC3_SRGB_BLOCK;
            case ImageFormat::BC4_UNORM:         return VK_FORMAT_BC4_UNORM_BLOCK;
            case ImageFormat::BC5_UNORM:         return VK_FORMAT_BC5_UNORM_BLOCK;
            case ImageFormat::BC7_UNORM:         return VK_FORMAT_BC7_UNORM_BLOCK;
            case ImageFormat::BC7_SRGB:          return VK_FORMAT_BC7_SRGB_BLOCK;

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

            case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return ImageFormat::BC1_RGBA_UNORM;
            case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:  return ImageFormat::BC1_RGBA_SRGB;
            case VK_FORMAT_BC3_UNORM_BLOCK:      return ImageFormat::BC3_UNORM;
            case VK_FORMAT_BC3_SRGB_BLOCK:       return ImageFormat::BC3_SRGB;
            case VK_FORMAT_BC4_UNORM_BLOCK:      return ImageFormat::BC4_UNORM;
            case VK_FORMAT_BC5_UNORM_BLOCK:      return ImageFormat::BC5_UNORM;
            case VK_FORMAT_BC7_UNORM_BLOCK:      return ImageFormat::BC7_UNORM;
            case VK_FORMAT_BC7_SRGB_BLOCK:       return ImageFormat::BC7_SRGB;

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

    VkSampleCountFlagBits ToVkSampleCount(SampleCount samples)
    {
        switch (samples) {
            case SampleCount::Two:     return VK_SAMPLE_COUNT_2_BIT;
            case SampleCount::Four:    return VK_SAMPLE_COUNT_4_BIT;
            case SampleCount::Eight:   return VK_SAMPLE_COUNT_8_BIT;
            case SampleCount::Sixteen: return VK_SAMPLE_COUNT_16_BIT;
            case SampleCount::One:
            default:                   return VK_SAMPLE_COUNT_1_BIT;
        }
    }

    VkCompareOp ToVkCompareOp(CompareOp op)
    {
        switch (op) {
            case CompareOp::Never:          return VK_COMPARE_OP_NEVER;
            case CompareOp::Less:           return VK_COMPARE_OP_LESS;
            case CompareOp::Equal:          return VK_COMPARE_OP_EQUAL;
            case CompareOp::LessOrEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
            case CompareOp::Greater:        return VK_COMPARE_OP_GREATER;
            case CompareOp::NotEqual:       return VK_COMPARE_OP_NOT_EQUAL;
            case CompareOp::GreaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case CompareOp::Always:
            default:                        return VK_COMPARE_OP_ALWAYS;
        }
    }

    VkStencilOp ToVkStencilOp(StencilOp op)
    {
        switch (op) {
            case StencilOp::Keep:           return VK_STENCIL_OP_KEEP;
            case StencilOp::Zero:           return VK_STENCIL_OP_ZERO;
            case StencilOp::Replace:        return VK_STENCIL_OP_REPLACE;
            case StencilOp::IncrementClamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
            case StencilOp::DecrementClamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
            case StencilOp::Invert:         return VK_STENCIL_OP_INVERT;
            case StencilOp::IncrementWrap:  return VK_STENCIL_OP_INCREMENT_AND_WRAP;
            case StencilOp::DecrementWrap:  return VK_STENCIL_OP_DECREMENT_AND_WRAP;
            default:                        return VK_STENCIL_OP_KEEP;
        }
    }

    VkBlendFactor ToVkBlendFactor(BlendFactor factor)
    {
        switch (factor) {
            case BlendFactor::Zero:             return VK_BLEND_FACTOR_ZERO;
            case BlendFactor::One:              return VK_BLEND_FACTOR_ONE;
            case BlendFactor::SrcColor:         return VK_BLEND_FACTOR_SRC_COLOR;
            case BlendFactor::OneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case BlendFactor::DstColor:         return VK_BLEND_FACTOR_DST_COLOR;
            case BlendFactor::OneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case BlendFactor::SrcAlpha:         return VK_BLEND_FACTOR_SRC_ALPHA;
            case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case BlendFactor::DstAlpha:         return VK_BLEND_FACTOR_DST_ALPHA;
            case BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            default:                            return VK_BLEND_FACTOR_ONE;
        }
    }

    VkBlendOp ToVkBlendOp(BlendOp op)
    {
        switch (op) {
            case BlendOp::Add:             return VK_BLEND_OP_ADD;
            case BlendOp::Subtract:        return VK_BLEND_OP_SUBTRACT;
            case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
            case BlendOp::Min:             return VK_BLEND_OP_MIN;
            case BlendOp::Max:             return VK_BLEND_OP_MAX;
            default:                       return VK_BLEND_OP_ADD;
        }
    }

    VkColorComponentFlags ToVkColorComponents(ColorComponent mask)
    {
        VkColorComponentFlags flags = 0;
        if (hasFlag(mask, ColorComponent::R)) flags |= VK_COLOR_COMPONENT_R_BIT;
        if (hasFlag(mask, ColorComponent::G)) flags |= VK_COLOR_COMPONENT_G_BIT;
        if (hasFlag(mask, ColorComponent::B)) flags |= VK_COLOR_COMPONENT_B_BIT;
        if (hasFlag(mask, ColorComponent::A)) flags |= VK_COLOR_COMPONENT_A_BIT;
        return flags;
    }

    VkCullModeFlags ToVkCullMode(CullMode mode)
    {
        switch (mode) {
            case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
            case CullMode::Back:  return VK_CULL_MODE_BACK_BIT;
            case CullMode::None:
            default:              return VK_CULL_MODE_NONE;
        }
    }

    VkFrontFace ToVkFrontFace(FrontFace face)
    {
        return (face == FrontFace::Clockwise) ? VK_FRONT_FACE_CLOCKWISE
                                              : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    }

    VkPolygonMode ToVkPolygonMode(PolygonMode mode)
    {
        return (mode == PolygonMode::Line) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    }

    VkStencilOpState ToVkStencilOpState(const StencilOpState& state)
    {
        VkStencilOpState result{};
        result.failOp = ToVkStencilOp(state.failOp);
        result.passOp = ToVkStencilOp(state.passOp);
        result.depthFailOp = ToVkStencilOp(state.depthFailOp);
        result.compareOp = ToVkCompareOp(state.compareOp);
        result.compareMask = state.compareMask;
        result.writeMask = state.writeMask;
        result.reference = state.reference;
        return result;
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
