#include "VulkanImage.hpp"

namespace dmrender {

    struct VulkanImageNativeData
    {
        VulkanSwapChain* swapChain = nullptr;
        uint32_t imageIndex = 0;
        VkImage image = VK_NULL_HANDLE;
        VkImageView imageView = VK_NULL_HANDLE;
        ImageFormat format = ImageFormat::Undefined;
        uint32_t width = 0;
        uint32_t height = 0;
        ImageUsage usage = ImageUsage::ColorTarget;
        ImageType type = ImageType::Image2D;
        std::string debugName;
    };

    VulkanImage::VulkanImage(VulkanSwapChain* swapChain,
                             uint32_t imageIndex,
                             VkImage image,
                             VkImageView imageView,
                             ImageFormat format,
                             uint32_t width,
                             uint32_t height,
                             ImageUsage usage,
                             ImageType type,
                             const std::string& debugName)
        : m_data(std::make_unique<VulkanImageNativeData>())
    {
        m_data->swapChain = swapChain;
        m_data->imageIndex = imageIndex;
        m_data->image = image;
        m_data->imageView = imageView;
        m_data->format = format;
        m_data->width = width;
        m_data->height = height;
        m_data->usage = usage;
        m_data->type = type;
        m_data->debugName = debugName;
    }

    VulkanImage::~VulkanImage() = default;

    uint32_t VulkanImage::width() const { return m_data->width; }
    uint32_t VulkanImage::height() const { return m_data->height; }
    uint32_t VulkanImage::depth() const { return 1; }
    uint32_t VulkanImage::mipLevels() const { return 1; }
    ImageFormat VulkanImage::format() const { return m_data->format; }
    ImageType VulkanImage::type() const { return m_data->type; }
    ImageUsage VulkanImage::usage() const { return m_data->usage; }

    void* VulkanImage::nativeHandle() const { return (void*)&m_data->image; }

    void* VulkanImage::nativeDrawableHandle() const { return (void*)m_data->swapChain; }

    const std::string& VulkanImage::debugName() const { return m_data->debugName; }

    void VulkanImage::setDebugName(const std::string& name) { m_data->debugName = name; }

    VkImageView VulkanImage::imageView() const { return m_data->imageView; }

    uint32_t VulkanImage::imageIndex() const { return m_data->imageIndex; }

    VulkanSwapChain* VulkanImage::swapChain() const { return m_data->swapChain; }

} // namespace dmrender
