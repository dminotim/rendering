#include "VulkanDevice.hpp"
#include <vulkan/vulkan.h>
#include <map>
#include <stdexcept>
#include <optional>
#include <tuple>
#include <render_vulkan/VulkanBuffer.hpp>
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
        /// @brief Render passes are immutable and cheap to share, so they are cached forever.
        std::map<RenderPassKey, VkRenderPass> renderPasses;
        uint32_t currentFrameSlot = 0;
    };

    bool RenderPassKey::operator<(const RenderPassKey& other) const
    {
        return std::tie(colorFormat, depthFormat, clearColor, clearDepth) <
               std::tie(other.colorFormat, other.depthFormat, other.clearColor, other.clearDepth);
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

        std::vector<VkAttachmentDescription> attachments;
        VkAttachmentReference colorRef{};
        VkAttachmentReference depthRef{};

        // Colour attachment. The final layout is PRESENT_SRC because every render pass this
        // backend builds targets a swapchain image; an offscreen path would key on that too.
        VkAttachmentDescription color{};
        color.format = key.colorFormat;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = key.clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = key.clearColor ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments.push_back(color);

        const bool hasDepth = key.depthFormat != VK_FORMAT_UNDEFINED;
        if (hasDepth) {
            VkAttachmentDescription depth{};
            depth.format = key.depthFormat;
            depth.samples = VK_SAMPLE_COUNT_1_BIT;
            depth.loadOp = key.clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth.stencilLoadOp = key.clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depth.initialLayout = key.clearDepth ? VK_IMAGE_LAYOUT_UNDEFINED
                                                 : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthRef.attachment = 1;
            depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachments.push_back(depth);
        }

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = hasDepth ? &depthRef : nullptr;

        // Wait for the presentation engine to be done with the image before writing it.
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        if (hasDepth) {
            dependency.srcStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dependency.dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dependency.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        }

        VkRenderPassCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 1;
        info.pDependencies = &dependency;

        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkCheck(vkCreateRenderPass(m_data->device, &info, nullptr, &renderPass), "vkCreateRenderPass");
        m_data->renderPasses.emplace(key, renderPass);
        return renderPass;
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

        VkPhysicalDeviceFeatures deviceFeatures{};
        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.pEnabledFeatures = &deviceFeatures;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();

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
   /*     vkGetDeviceQueue(device, graphicsFamilyIndex, 0, &graphicsQueue);
        vkGetDeviceQueue(device, presentFamilyIndex, 0, &presentQueue);*/
    }
}