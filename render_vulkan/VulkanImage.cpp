#include "VulkanImage.hpp"

#include <stdexcept>

#include <render_vulkan/VulkanDevice.hpp>
#include <render_vulkan/VulkanUtils.hpp>

namespace dmrender {

    struct VulkanImageNativeData
    {
        VulkanDevice* device = nullptr;          ///< Set only when this object owns its image.
        VulkanSwapChain* swapChain = nullptr;    ///< Set only for swapchain images.
        uint32_t imageIndex = 0;

        VkImage image = VK_NULL_HANDLE;
        VkImageView imageView = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;  ///< Owned images only.
        bool owning = false;

        VkImageLayout restingLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        ImageFormat format = ImageFormat::Undefined;
        uint32_t width = 0;
        uint32_t height = 0;
        ImageUsage usage = ImageUsage::ColorTarget;
        ImageType type = ImageType::Image2D;
        std::string debugName;
    };

    namespace {

        VkImageType toVkImageType(ImageType type)
        {
            switch (type) {
                case ImageType::Image1D: return VK_IMAGE_TYPE_1D;
                case ImageType::Image3D: return VK_IMAGE_TYPE_3D;
                case ImageType::Image2D:
                case ImageType::CubeMap:
                default:                 return VK_IMAGE_TYPE_2D;
            }
        }

        VkImageViewType toVkImageViewType(ImageType type)
        {
            switch (type) {
                case ImageType::Image1D: return VK_IMAGE_VIEW_TYPE_1D;
                case ImageType::Image3D: return VK_IMAGE_VIEW_TYPE_3D;
                case ImageType::CubeMap: return VK_IMAGE_VIEW_TYPE_CUBE;
                case ImageType::Image2D:
                default:                 return VK_IMAGE_VIEW_TYPE_2D;
            }
        }

    } // namespace

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
        m_data->owning = false;
        m_data->restingLayout = RestingLayoutFor(usage, /*isSwapChainImage=*/true);
        m_data->format = format;
        m_data->width = width;
        m_data->height = height;
        m_data->usage = usage;
        m_data->type = type;
        m_data->debugName = debugName;
    }

    VulkanImage::VulkanImage(VulkanDevice* device,
                             ImageType type,
                             ImageFormat format,
                             uint32_t width,
                             uint32_t height,
                             ImageUsage usage,
                             const std::string& debugName)
        : m_data(std::make_unique<VulkanImageNativeData>())
    {
        if (!device) throw std::runtime_error("VulkanImage: null device");
        if (width == 0 || height == 0) throw std::runtime_error("VulkanImage: zero sized image");

        m_data->device = device;
        m_data->owning = true;
        m_data->restingLayout = RestingLayoutFor(usage, /*isSwapChainImage=*/false);
        m_data->format = format;
        m_data->width = width;
        m_data->height = height;
        m_data->usage = usage;
        m_data->type = type;
        m_data->debugName = debugName;

        VkDevice logicalDevice = device->logicalDevice();
        const VkFormat vkFormat = ToVkFormat(format);

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = toVkImageType(type);
        imageInfo.format = vkFormat;
        imageInfo.extent = { width, height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = (type == ImageType::CubeMap) ? 6 : 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = ToVkImageUsage(usage);
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        // Every render pass that writes this image either clears it — in which case the contents
        // are discarded and the initial layout is UNDEFINED anyway — or expects it in its resting
        // layout, which a previous pass will have left it in.
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (type == ImageType::CubeMap) {
            imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        }

        VkCheck(vkCreateImage(logicalDevice, &imageInfo, nullptr, &m_data->image), "vkCreateImage");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(logicalDevice, m_data->image, &requirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = FindMemoryType(device->physicalDevice(),
                                                   requirements.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkCheck(vkAllocateMemory(logicalDevice, &allocInfo, nullptr, &m_data->memory), "vkAllocateMemory");
        VkCheck(vkBindImageMemory(logicalDevice, m_data->image, m_data->memory, 0), "vkBindImageMemory");

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_data->image;
        viewInfo.viewType = toVkImageViewType(type);
        viewInfo.format = vkFormat;
        viewInfo.subresourceRange.aspectMask = IsDepthFormat(format)
            ? VK_IMAGE_ASPECT_DEPTH_BIT
            : VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = imageInfo.arrayLayers;
        VkCheck(vkCreateImageView(logicalDevice, &viewInfo, nullptr, &m_data->imageView), "vkCreateImageView");
    }

    VulkanImage::~VulkanImage()
    {
        if (!m_data->owning) return;

        VkDevice logicalDevice = m_data->device->logicalDevice();
        // See VulkanBuffer's destructor: Vulkan will not keep this alive for an in-flight frame.
        vkDeviceWaitIdle(logicalDevice);

        // Framebuffers built from this view must go first — they hold it by handle.
        m_data->device->invalidateFramebuffersUsing(m_data->imageView);

        if (m_data->imageView) vkDestroyImageView(logicalDevice, m_data->imageView, nullptr);
        if (m_data->image) vkDestroyImage(logicalDevice, m_data->image, nullptr);
        if (m_data->memory) vkFreeMemory(logicalDevice, m_data->memory, nullptr);
    }

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

    VkImageLayout VulkanImage::restingLayout() const { return m_data->restingLayout; }

} // namespace dmrender
