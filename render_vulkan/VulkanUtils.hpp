//
// Created by Artem Avdoshkin on 14.08.2025.
//
// Small helpers shared by the whole Vulkan backend plus the ImGui glue that
// mirrors MetalUtils.hpp on the Apple side.
//

#ifndef RENDERING_VULKANUTILS_HPP
#define RENDERING_VULKANUTILS_HPP

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>

#include "GImage.hpp"
#include "GSampler.hpp"

namespace dmrender {

    class Device;
    class SwapChain;
    class RenderPassDescriptor;
    class CommandBuffer;

    /**
     * @brief How many frames the CPU is allowed to record ahead of the GPU.
     *
     * This single constant drives three things at once, so they can never drift apart:
     *  - the number of command buffers / fences / descriptor pools owned by VulkanCommandQueues,
     *  - the number of "image available" semaphores owned by VulkanSwapChain,
     *  - the number of regions inside a BufferUsage::Dynamic VulkanBuffer.
     */
    inline constexpr uint32_t kFramesInFlight = 2;

    /**
     * @brief Highest buffer binding slot the backend reserves in descriptor set 0.
     *
     * Slot 0 is the vertex storage buffer, slots 1..kMaxBindingSlots-1 are uniform buffers.
     */
    inline constexpr uint32_t kMaxBindingSlots = 8;

    /**
     * @brief Highest texture binding slot the backend reserves in descriptor set 1.
     *
     * Textures live in their own descriptor set because they live in their own numbering space
     * in the interface — `setTexture(0, …)` and `setUniformBuffer(0, …)` are different bindings,
     * matching Metal's separate `[[texture(n)]]` and `[[buffer(n)]]` indices.
     */
    inline constexpr uint32_t kMaxTextureSlots = 8;

    /// @brief Descriptor set index holding buffers (`set = 0` in GLSL).
    inline constexpr uint32_t kBufferDescriptorSet = 0;

    /// @brief Descriptor set index holding combined image samplers (`set = 1` in GLSL).
    inline constexpr uint32_t kTextureDescriptorSet = 1;

    /// @brief Converts an abstract sampler filter to its Vulkan counterpart.
    VkFilter ToVkFilter(SamplerFilter filter);

    /// @brief Converts an abstract address mode to its Vulkan counterpart.
    VkSamplerAddressMode ToVkAddressMode(SamplerAddressMode mode);

    /// @brief The Vulkan usage flags implied by an abstract ImageUsage bitmask.
    VkImageUsageFlags ToVkImageUsage(ImageUsage usage);

    /**
     * @brief The layout an image is expected to be in outside of a render pass.
     *
     * Every render pass this backend builds leaves its attachments in their resting layout and,
     * when it does not clear them, expects to find them in it. Tracking one layout per image
     * rather than a full state machine is enough because the interface only exposes two
     * transitions: "written by a render pass" and "read by a sampler".
     */
    VkImageLayout RestingLayoutFor(ImageUsage usage, bool isSwapChainImage);

    /// @brief Throws std::runtime_error with @p what appended when @p result is not VK_SUCCESS.
    void VkCheck(VkResult result, const char* what);

    /// @brief Translates an abstract ImageFormat into its Vulkan counterpart.
    VkFormat ToVkFormat(ImageFormat format);

    /// @brief Translates a Vulkan format back into the abstract enum (Undefined when unknown).
    ImageFormat FromVkFormat(VkFormat format);

    /// @brief True for formats that carry depth and/or stencil data.
    bool IsDepthFormat(ImageFormat format);

    /// @brief Picks a memory type index satisfying @p typeFilter and @p properties.
    uint32_t FindMemoryType(VkPhysicalDevice physicalDevice,
                            uint32_t typeFilter,
                            VkMemoryPropertyFlags properties);

    // --- ImGui glue (counterpart of InitImguiMetal & friends) ---

    bool InitImguiVulkan(const std::shared_ptr<SwapChain>& swapChain);

    bool NewFrameImguiVulkan(const std::shared_ptr<RenderPassDescriptor>& passDesc);

    bool RenderInternalImguiVulkan(const std::shared_ptr<CommandBuffer>& cmdBuffer);

    bool ShutdownImguiVulkan();

} // namespace dmrender

#endif //RENDERING_VULKANUTILS_HPP
