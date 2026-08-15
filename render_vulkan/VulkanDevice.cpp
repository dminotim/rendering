#include "VulkanDevice.hpp"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <utility>
#include <stdexcept>
#include <optional>
#include <tuple>
#include <render_vulkan/VulkanBuffer.hpp>
#include <render_vulkan/VulkanImage.hpp>
#include <render_vulkan/VulkanSampler.hpp>
#include <render_vulkan/VulkanSingleton.hpp>
#include <render_vulkan/VulkanUtils.hpp>

namespace dmrender {

    struct VulkanDeviceNativeData
    {
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        uint32_t graphicsFamilyIndex = 0;
        uint32_t presentFamilyIndex = 0;
        VkPhysicalDeviceProperties properties{};
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        bool memoryBudgetExtension = false;

        // --- Transfer context: everything needed to push data into device-local memory ---
        VkQueue transferQueue = VK_NULL_HANDLE;   ///< Same queue the graphics work uses.
        VkCommandPool transferPool = VK_NULL_HANDLE;
        VkCommandBuffer transferCmd = VK_NULL_HANDLE;
        VkFence transferFence = VK_NULL_HANDLE;
        /// Reused across uploads and grown on demand, so repeated uploads do not re-allocate.
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        void* stagingMapped = nullptr;
        VkDeviceSize stagingCapacity = 0;

        /// @brief Render passes are immutable and cheap to share, so they are cached forever.
        std::map<RenderPassKey, VkRenderPass> renderPasses;
        /// @brief Framebuffers, keyed by the render pass and the exact set of views they wrap.
        std::map<std::pair<VkRenderPass, std::vector<VkImageView>>, VkFramebuffer> framebuffers;
        uint32_t currentFrameSlot = 0;
    };

    bool RenderPassAttachmentKey::operator<(const RenderPassAttachmentKey& other) const
    {
        return std::tie(format, clear, restingLayout) <
               std::tie(other.format, other.clear, other.restingLayout);
    }

    bool RenderPassKey::operator<(const RenderPassKey& other) const
    {
        if (colors.size() != other.colors.size()) return colors.size() < other.colors.size();
        for (size_t i = 0; i < colors.size(); ++i) {
            if (colors[i] < other.colors[i]) return true;
            if (other.colors[i] < colors[i]) return false;
        }
        return depth < other.depth;
    }

    struct QueueFamilyIndices {
        std::optional<uint32_t> graphicsFamily;
        std::optional<uint32_t> presentFamily;

        bool isComplete() const {
            return graphicsFamily.has_value() && presentFamily.has_value();
        }
    };

    const std::vector<const char*> validationLayers = {
        "VK_LAYER_KHRONOS_validation"
    };

    const std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    /// Extensions the backend uses when present but works without.
    const std::vector<const char*> optionalDeviceExtensions = {
        // Turns queryMemoryBudget() from "heap sizes only" into real per-process usage figures.
        VK_EXT_MEMORY_BUDGET_EXTENSION_NAME
    };

    /// @brief Names of the optional extensions @p p_device actually supports.
    std::vector<const char*> findSupportedOptionalExtensions(VkPhysicalDevice p_device)
    {
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(p_device, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> available(extensionCount);
        vkEnumerateDeviceExtensionProperties(p_device, nullptr, &extensionCount, available.data());

        std::vector<const char*> supported;
        for (const char* wanted : optionalDeviceExtensions) {
            for (const auto& extension : available) {
                if (std::strcmp(wanted, extension.extensionName) == 0) {
                    supported.push_back(wanted);
                    break;
                }
            }
        }
        return supported;
    }

    struct SwapChainSupportDetails {
        VkSurfaceCapabilitiesKHR capabilities;
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice p_device, const std::shared_ptr<dmrender::Surface>& surface) {
        SwapChainSupportDetails details;
        VkSurfaceKHR vulkanSurface = *((VkSurfaceKHR*)surface->nativeHandle());
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p_device, vulkanSurface, &details.capabilities);

        uint32_t formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(p_device, vulkanSurface, &formatCount, nullptr);
        if (formatCount != 0) {
            details.formats.resize(formatCount);
            vkGetPhysicalDeviceSurfaceFormatsKHR(p_device, vulkanSurface, &formatCount, details.formats.data());
        }

        uint32_t presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(p_device, vulkanSurface, &presentModeCount, nullptr);
        if (presentModeCount != 0) {
            details.presentModes.resize(presentModeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(p_device, vulkanSurface, &presentModeCount, details.presentModes.data());
        }
        return details;
    }

    bool checkDeviceExtensionSupport(VkPhysicalDevice p_device) {
        uint32_t extensionCount;
        vkEnumerateDeviceExtensionProperties(p_device, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(p_device, nullptr, &extensionCount, availableExtensions.data());

        std::set<std::string> required(deviceExtensions.begin(), deviceExtensions.end());
        for (const auto& ext : availableExtensions) {
            required.erase(ext.extensionName);
        }
        return required.empty();
    }

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice p_device, const std::shared_ptr<dmrender::Surface>& surface) {
        QueueFamilyIndices indices;
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(p_device, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(p_device, &queueFamilyCount, queueFamilies.data());
        VkSurfaceKHR vulkanSurface = *((VkSurfaceKHR*)surface->nativeHandle());
        int i = 0;
        for (const auto& queueFamily : queueFamilies) {
            if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                indices.graphicsFamily = i;
            }
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(p_device, i, vulkanSurface, &presentSupport);
            if (presentSupport) {
                indices.presentFamily = i;
            }
            if (indices.isComplete()) break;
            i++;
        }
        return indices;
    }

    bool isDeviceSuitable(VkPhysicalDevice p_device, const std::shared_ptr<dmrender::Surface>& surface) {
        QueueFamilyIndices indices = findQueueFamilies(p_device, surface);
        bool extensionsSupported = checkDeviceExtensionSupport(p_device);
        bool swapChainAdequate = false;
        if (extensionsSupported) {
            SwapChainSupportDetails swapChainSupport = querySwapChainSupport(p_device, surface);
            swapChainAdequate = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
        }
        return indices.isComplete() && extensionsSupported && swapChainAdequate;
    }

    std::shared_ptr<Device> VulkanDevice::createDefaultDevice(const std::shared_ptr<Surface>& surface)
    {
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(VulkanSingleton::getInstance().nativeHandle(), &deviceCount, nullptr);
        if (deviceCount == 0) throw std::runtime_error("Failed to find GPUs with Vulkan support!");

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(VulkanSingleton::getInstance().nativeHandle(), &deviceCount, devices.data());

        for (const auto& dev : devices) {
            if (isDeviceSuitable(dev, surface)) {
                physicalDevice = dev;
                break;
            }
        }
        if (physicalDevice == VK_NULL_HANDLE) throw std::runtime_error("Failed to find a suitable GPU!");

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physicalDevice, &props);
        std::string deviceName = props.deviceName;
        uint32_t deviceID = props.deviceID;
        DeviceId dId{ .name = deviceName, .id = deviceID };
        return std::make_shared<VulkanDevice>(surface, dId, (void*)&physicalDevice);
    }

    std::shared_ptr<Device>  VulkanDevice::createDeviceById(const std::shared_ptr<Surface>& surface, const DeviceId& id)
    {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(VulkanSingleton::getInstance().nativeHandle(), &deviceCount, nullptr);
        if (deviceCount == 0) throw std::runtime_error("Failed to find GPUs with Vulkan support!");

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(VulkanSingleton::getInstance().nativeHandle(), &deviceCount, devices.data());

        for (const auto& dev : devices) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(dev, &props);
            std::string deviceName = props.deviceName;
            uint32_t deviceID = props.deviceID;
            if (deviceName == id.name && id.id == deviceID)
                return std::make_shared<VulkanDevice>(surface, id, (void*)&dev);
        }
        throw std::runtime_error("Failed to find a suitable GPU!");
    }

    std::vector<DeviceId> VulkanDevice::enumerateAvailableDevices()
    {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(VulkanSingleton::getInstance().nativeHandle(), &deviceCount, nullptr);
        if (deviceCount == 0) throw std::runtime_error("Failed to find GPUs with Vulkan support!");
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(VulkanSingleton::getInstance().nativeHandle(), &deviceCount, devices.data());
        std::vector<DeviceId> result;
        for (const auto& dev : devices) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(dev, &props);
            std::string deviceName = props.deviceName;
            uint32_t deviceID = props.deviceID;
            result.push_back(DeviceId{ .name = deviceName, .id = deviceID });
        }
        return result;
    }

    VulkanDevice::~VulkanDevice()
    {
        if (!m_data->device)
            return;

        vkDeviceWaitIdle(m_data->device);

        if (m_data->stagingMapped) vkUnmapMemory(m_data->device, m_data->stagingMemory);
        if (m_data->stagingBuffer) vkDestroyBuffer(m_data->device, m_data->stagingBuffer, nullptr);
        if (m_data->stagingMemory) vkFreeMemory(m_data->device, m_data->stagingMemory, nullptr);
        if (m_data->transferFence) vkDestroyFence(m_data->device, m_data->transferFence, nullptr);
        // Destroying the pool frees the transfer command buffer allocated from it.
        if (m_data->transferPool) vkDestroyCommandPool(m_data->device, m_data->transferPool, nullptr);

        for (auto& [key, framebuffer] : m_data->framebuffers) {
            vkDestroyFramebuffer(m_data->device, framebuffer, nullptr);
        }
        m_data->framebuffers.clear();
        for (auto& [key, renderPass] : m_data->renderPasses) {
            vkDestroyRenderPass(m_data->device, renderPass, nullptr);
        }
        m_data->renderPasses.clear();
        vkDestroyDevice(m_data->device, nullptr);
        m_data->device = VK_NULL_HANDLE;
    }

    bool VulkanDevice::activateExtension(DeviceExtension /*ext*/)
    {
        // cant activate extention after creation
        return false;
    }
    bool VulkanDevice::isExtensionAvailable(DeviceExtension ext) const
    {
        return m_activatedInstanceExtensions.contains(ext);
    }

    std::shared_ptr<GBuffer> VulkanDevice::createBuffer(
        BufferType type,
        BufferUsage usage,
        size_t size,
        const void* initialData,
        const std::string& debugName)
    {
        return std::make_shared<VulkanBuffer>(this, type, usage, size, initialData, debugName);
    }

    void* VulkanDevice::nativeHandle() const
    {
        return (void*)&m_data->physicalDevice;
    }

    void* VulkanDevice::getLogicalDevice() const
    {
        return (void*)&m_data->device;
    }

    VkPhysicalDevice VulkanDevice::physicalDevice() const
    {
        return m_data->physicalDevice;
    }

    VkDevice VulkanDevice::logicalDevice() const
    {
        return m_data->device;
    }

    const VkPhysicalDeviceProperties& VulkanDevice::properties() const
    {
        return m_data->properties;
    }

    uint32_t VulkanDevice::currentFrameSlot() const
    {
        return m_data->currentFrameSlot;
    }

    void VulkanDevice::setCurrentFrameSlot(uint32_t slot)
    {
        m_data->currentFrameSlot = slot;
    }

    VkRenderPass VulkanDevice::acquireRenderPass(const RenderPassKey& key)
    {
        if (auto it = m_data->renderPasses.find(key); it != m_data->renderPasses.end()) {
            return it->second;
        }

        if (key.colors.empty()) {
            throw std::runtime_error("acquireRenderPass: a render pass needs at least one colour attachment");
        }

        std::vector<VkAttachmentDescription> attachments;
        std::vector<VkAttachmentReference> colorRefs;
        attachments.reserve(key.colors.size() + 1);
        colorRefs.reserve(key.colors.size());

        // One description per colour attachment. Each carries its own resting layout, so a pass
        // writing a swapchain image alongside an offscreen texture ends with the first in
        // PRESENT_SRC_KHR and the second in SHADER_READ_ONLY_OPTIMAL, ready to be sampled.
        for (const RenderPassAttachmentKey& color : key.colors) {
            VkAttachmentDescription description{};
            description.format = color.format;
            description.samples = VK_SAMPLE_COUNT_1_BIT;
            description.loadOp = color.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            description.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            description.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            // Clearing discards whatever was there, so the previous layout does not matter and
            // UNDEFINED lets the driver skip a decompress.
            description.initialLayout = color.clear ? VK_IMAGE_LAYOUT_UNDEFINED : color.restingLayout;
            description.finalLayout = color.restingLayout;

            VkAttachmentReference ref{};
            ref.attachment = static_cast<uint32_t>(attachments.size());
            ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            attachments.push_back(description);
            colorRefs.push_back(ref);
        }

        VkAttachmentReference depthRef{};
        const bool hasDepth = key.depth.format != VK_FORMAT_UNDEFINED;
        if (hasDepth) {
            VkAttachmentDescription description{};
            description.format = key.depth.format;
            description.samples = VK_SAMPLE_COUNT_1_BIT;
            description.loadOp = key.depth.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            description.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            description.stencilLoadOp = key.depth.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                        : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            description.initialLayout = key.depth.clear ? VK_IMAGE_LAYOUT_UNDEFINED : key.depth.restingLayout;
            description.finalLayout = key.depth.restingLayout;

            depthRef.attachment = static_cast<uint32_t>(attachments.size());
            depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachments.push_back(description);
        }

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = static_cast<uint32_t>(colorRefs.size());
        subpass.pColorAttachments = colorRefs.data();
        subpass.pDepthStencilAttachment = hasDepth ? &depthRef : nullptr;

        // Two external dependencies bracket the subpass, and between them they remove the need
        // for any explicit barrier around an offscreen target:
        //
        //   [in]  nothing may write these attachments until the presentation engine has released
        //         them and until any earlier pass that sampled them has finished reading.
        //   [out] the colour writes must be visible to a later pass's fragment shader, which is
        //         what makes "render to a texture, then sample it" work inside one command buffer.
        VkSubpassDependency dependencies[2]{};

        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        if (hasDepth) {
            const VkPipelineStageFlags depthStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            dependencies[0].srcStageMask |= depthStages;
            dependencies[0].dstStageMask |= depthStages;
            dependencies[0].dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dependencies[1].srcStageMask |= depthStages;
            dependencies[1].srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        }

        VkRenderPassCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 2;
        info.pDependencies = dependencies;

        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkCheck(vkCreateRenderPass(m_data->device, &info, nullptr, &renderPass), "vkCreateRenderPass");
        m_data->renderPasses.emplace(key, renderPass);
        return renderPass;
    }

    VkFramebuffer VulkanDevice::acquireFramebuffer(VkRenderPass renderPass,
                                                   const std::vector<VkImageView>& attachments,
                                                   uint32_t width,
                                                   uint32_t height)
    {
        const auto key = std::make_pair(renderPass, attachments);
        if (auto it = m_data->framebuffers.find(key); it != m_data->framebuffers.end()) {
            return it->second;
        }

        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = renderPass;
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.width = width;
        info.height = height;
        info.layers = 1;

        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkCheck(vkCreateFramebuffer(m_data->device, &info, nullptr, &framebuffer), "vkCreateFramebuffer");
        m_data->framebuffers.emplace(key, framebuffer);
        return framebuffer;
    }

    void VulkanDevice::invalidateFramebuffersUsing(VkImageView view)
    {
        for (auto it = m_data->framebuffers.begin(); it != m_data->framebuffers.end(); ) {
            const std::vector<VkImageView>& attachments = it->first.second;
            const bool referencesView =
                std::find(attachments.begin(), attachments.end(), view) != attachments.end();

            if (referencesView) {
                vkDestroyFramebuffer(m_data->device, it->second, nullptr);
                it = m_data->framebuffers.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::shared_ptr<GImage> VulkanDevice::createImage(
        ImageType type,
        ImageFormat format,
        uint32_t width,
        uint32_t height,
        ImageUsage usage,
        const std::string& debugName)
    {
        return std::make_shared<VulkanImage>(this, type, format, width, height, usage, debugName);
    }

    std::shared_ptr<GSampler> VulkanDevice::createSampler(
        const SamplerDesc& desc,
        const std::string& debugName)
    {
        return std::make_shared<VulkanSampler>(this, desc, debugName);
    }

    bool VulkanDevice::hasMemoryBudgetExtension() const
    {
        return m_data->memoryBudgetExtension;
    }

    MemoryBudget VulkanDevice::queryMemoryBudget() const
    {
        MemoryBudget budget{};
        budget.preciseBudget = m_data->memoryBudgetExtension;

        // VK_EXT_memory_budget chains onto the standard memory properties query and fills in per
        // heap budget/usage figures that account for other processes. Without it the only honest
        // answer is the heap size.
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budgetProperties{};
        budgetProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;

        VkPhysicalDeviceMemoryProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        if (m_data->memoryBudgetExtension) {
            properties.pNext = &budgetProperties;
        }
        vkGetPhysicalDeviceMemoryProperties2(m_data->physicalDevice, &properties);

        const VkPhysicalDeviceMemoryProperties& memory = properties.memoryProperties;

        // Sum every device-local heap. Discrete GPUs normally have exactly one; integrated ones
        // report their shared system memory as device-local, which is what unifiedMemory means.
        bool sawHostVisibleDeviceLocal = false;
        for (uint32_t heapIndex = 0; heapIndex < memory.memoryHeapCount; ++heapIndex) {
            if ((memory.memoryHeaps[heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) {
                continue;
            }

            budget.deviceLocalTotalBytes += memory.memoryHeaps[heapIndex].size;
            if (m_data->memoryBudgetExtension) {
                budget.deviceLocalBudgetBytes += budgetProperties.heapBudget[heapIndex];
                budget.deviceLocalUsedBytes += budgetProperties.heapUsage[heapIndex];
            } else {
                // No usage figure is available, so report the heap as entirely free rather than
                // inventing a number. preciseBudget tells the caller not to trust it.
                budget.deviceLocalBudgetBytes += memory.memoryHeaps[heapIndex].size;
            }
        }

        for (uint32_t typeIndex = 0; typeIndex < memory.memoryTypeCount; ++typeIndex) {
            const VkMemoryPropertyFlags flags = memory.memoryTypes[typeIndex].propertyFlags;
            if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                sawHostVisibleDeviceLocal = true;
                break;
            }
        }
        // A device whose device-local memory is directly CPU-writable either shares one physical
        // pool with the host or exposes all of VRAM over the bus (resizable BAR). Either way a
        // staging copy is avoidable, which is exactly what VulkanBuffer checks for.
        budget.unifiedMemory = sawHostVisibleDeviceLocal &&
                               m_data->properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

        return budget;
    }

    uint32_t VulkanDevice::selectMemoryType(uint32_t typeBits,
                                            VkMemoryPropertyFlags preferred,
                                            VkMemoryPropertyFlags required,
                                            VkMemoryPropertyFlags& outFlags) const
    {
        const VkPhysicalDeviceMemoryProperties& memory = m_data->memoryProperties;

        for (VkMemoryPropertyFlags wanted : { preferred, required }) {
            for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
                const bool typeAllowed = (typeBits & (1u << i)) != 0;
                const VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
                if (typeAllowed && (flags & wanted) == wanted) {
                    outFlags = flags;
                    return i;
                }
            }
        }
        throw std::runtime_error("selectMemoryType: no memory type satisfies the requested properties");
    }

    void VulkanDevice::uploadToDeviceLocalBuffer(VkBuffer destination,
                                                 VkDeviceSize destinationOffset,
                                                 const void* data,
                                                 VkDeviceSize size)
    {
        if (!data || size == 0) return;

        // Grow the shared staging buffer if this upload does not fit. Uploads are rare and their
        // sizes cluster, so one buffer that reaches a high-water mark beats allocating per call.
        if (size > m_data->stagingCapacity) {
            if (m_data->stagingMapped) {
                vkUnmapMemory(m_data->device, m_data->stagingMemory);
                m_data->stagingMapped = nullptr;
            }
            if (m_data->stagingBuffer) vkDestroyBuffer(m_data->device, m_data->stagingBuffer, nullptr);
            if (m_data->stagingMemory) vkFreeMemory(m_data->device, m_data->stagingMemory, nullptr);
            m_data->stagingBuffer = VK_NULL_HANDLE;
            m_data->stagingMemory = VK_NULL_HANDLE;

            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = size;
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VkCheck(vkCreateBuffer(m_data->device, &bufferInfo, nullptr, &m_data->stagingBuffer),
                    "vkCreateBuffer (staging)");

            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(m_data->device, m_data->stagingBuffer, &requirements);

            VkMemoryPropertyFlags chosenFlags = 0;
            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = requirements.size;
            allocInfo.memoryTypeIndex = selectMemoryType(
                requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                chosenFlags);
            VkCheck(vkAllocateMemory(m_data->device, &allocInfo, nullptr, &m_data->stagingMemory),
                    "vkAllocateMemory (staging)");
            VkCheck(vkBindBufferMemory(m_data->device, m_data->stagingBuffer, m_data->stagingMemory, 0),
                    "vkBindBufferMemory (staging)");
            VkCheck(vkMapMemory(m_data->device, m_data->stagingMemory, 0, VK_WHOLE_SIZE, 0, &m_data->stagingMapped),
                    "vkMapMemory (staging)");

            m_data->stagingCapacity = requirements.size;
        }

        std::memcpy(m_data->stagingMapped, data, static_cast<size_t>(size));

        VkCheck(vkResetCommandBuffer(m_data->transferCmd, 0), "vkResetCommandBuffer (transfer)");

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkCheck(vkBeginCommandBuffer(m_data->transferCmd, &beginInfo), "vkBeginCommandBuffer (transfer)");

        VkBufferCopy region{};
        region.srcOffset = 0;
        region.dstOffset = destinationOffset;
        region.size = size;
        vkCmdCopyBuffer(m_data->transferCmd, m_data->stagingBuffer, destination, 1, &region);

        // Make the copy visible to every stage that might read the buffer afterwards. A single
        // barrier here is cheaper than reasoning about which one this particular buffer needs.
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                                VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(m_data->transferCmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);

        VkCheck(vkEndCommandBuffer(m_data->transferCmd), "vkEndCommandBuffer (transfer)");

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &m_data->transferCmd;

        VkCheck(vkResetFences(m_data->device, 1, &m_data->transferFence), "vkResetFences (transfer)");
        VkCheck(vkQueueSubmit(m_data->transferQueue, 1, &submitInfo, m_data->transferFence),
                "vkQueueSubmit (transfer)");
        // Waiting keeps the staging buffer safe to overwrite on the next call and lets the caller
        // treat createBuffer() as "the data is there when this returns".
        VkCheck(vkWaitForFences(m_data->device, 1, &m_data->transferFence, VK_TRUE, UINT64_MAX),
                "vkWaitForFences (transfer)");
    }

    uint32_t VulkanDevice::getGraphicsFamilyIndex() const
    {
        return m_data->graphicsFamilyIndex;
    }

    uint32_t VulkanDevice::getPresentFamilyIndex() const
    {
        return m_data->presentFamilyIndex;
    }

    VulkanDevice::VulkanDevice(const std::shared_ptr<Surface>& surface, DeviceId id, void* nativeDevice)
        :Device(id), m_data(std::make_unique<VulkanDeviceNativeData>())
    {
        m_data->physicalDevice = *static_cast<VkPhysicalDevice*>(nativeDevice);
        vkGetPhysicalDeviceProperties(m_data->physicalDevice, &m_data->properties);

        QueueFamilyIndices indices = findQueueFamilies(m_data->physicalDevice, surface);
        if (!indices.isComplete())
            throw std::runtime_error("Queue families is not complite");

        m_data->graphicsFamilyIndex = indices.graphicsFamily.value();
        m_data->presentFamilyIndex = indices.presentFamily.value();

        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        std::set<uint32_t> uniqueFamilies = { m_data->graphicsFamilyIndex, m_data->presentFamilyIndex };
        float queuePriority = 1.0f;
        for (uint32_t family : uniqueFamilies) {
            VkDeviceQueueCreateInfo queueCreateInfo{};
            queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = family;
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(queueCreateInfo);
        }

        // Required extensions plus whichever optional ones this GPU happens to offer.
        std::vector<const char*> enabledExtensions = deviceExtensions;
        for (const char* extension : findSupportedOptionalExtensions(m_data->physicalDevice)) {
            enabledExtensions.push_back(extension);
            if (std::strcmp(extension, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) {
                m_data->memoryBudgetExtension = true;
            }
        }

        VkPhysicalDeviceFeatures deviceFeatures{};
        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.pEnabledFeatures = &deviceFeatures;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
        createInfo.ppEnabledExtensionNames = enabledExtensions.data();

        if (ENABLE_VALIDATION_LAYER) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
            createInfo.ppEnabledLayerNames = validationLayers.data();
            m_activatedInstanceExtensions.insert(DeviceExtension::Validation);
        }

        if (vkCreateDevice(m_data->physicalDevice, &createInfo, nullptr, &m_data->device) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create logical device!");
        }
        m_activatedInstanceExtensions.insert(DeviceExtension::Surface);
        m_activatedInstanceExtensions.insert(DeviceExtension::SwapChain);

        vkGetPhysicalDeviceMemoryProperties(m_data->physicalDevice, &m_data->memoryProperties);

        // Transfer context. The queue handle is the same object VulkanCommandQueues will fetch —
        // vkGetDeviceQueue returns the identical queue for the same family and index — so uploads
        // are ordered against rendering work rather than racing it. Single-threaded use only.
        vkGetDeviceQueue(m_data->device, m_data->graphicsFamilyIndex, 0, &m_data->transferQueue);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = m_data->graphicsFamilyIndex;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                         VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        VkCheck(vkCreateCommandPool(m_data->device, &poolInfo, nullptr, &m_data->transferPool),
                "vkCreateCommandPool (transfer)");

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_data->transferPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VkCheck(vkAllocateCommandBuffers(m_data->device, &allocInfo, &m_data->transferCmd),
                "vkAllocateCommandBuffers (transfer)");

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkCheck(vkCreateFence(m_data->device, &fenceInfo, nullptr, &m_data->transferFence),
                "vkCreateFence (transfer)");
   /*     vkGetDeviceQueue(device, graphicsFamilyIndex, 0, &graphicsQueue);
        vkGetDeviceQueue(device, presentFamilyIndex, 0, &presentQueue);*/
    }
}