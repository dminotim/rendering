
#include <vector>
#include <vulkan/vulkan_core.h>

class VulkanSingleton {
public:
    static VulkanSingleton& getInstance();
    VulkanSingleton(const VulkanSingleton&) = delete;
    VulkanSingleton& operator=(const VulkanSingleton&) = delete;
    VkInstance nativeHandle() const;

private:
    VulkanSingleton();
    ~VulkanSingleton();
    static const std::vector<const char*> validationLayers;
    bool checkValidationLayerSupport();
    VkInstance m_instance = nullptr;
    VkDebugUtilsMessengerEXT m_debugMessenger = nullptr;
};