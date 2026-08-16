#include "VulkanImage.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>

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
        VulkanAllocation allocation{};           ///< Owned images only.
        bool owning = false;

        VkImageLayout restingLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        ImageFormat format = ImageFormat::Undefined;
        uint32_t width = 0;
        uint32_t height = 0;
        ImageUsage usage = ImageUsage::ColorTarget;
        ImageType type = ImageType::Image2D;
        uint32_t depth = 1;
        uint32_t arrayLayers = 1;
        uint32_t mipLevels = 1;
        SampleCount sampleCount = SampleCount::One;
        /// Single-layer views, created on demand so a pass can target one cube face or cascade.
        std::map<uint32_t, VkImageView> layerViews;
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

        /// @brief Turns a requested level count into a concrete one, expanding kFullMipChain.
        uint32_t resolveMipLevels(uint32_t requested, uint32_t width, uint32_t height, uint32_t depth)
        {
            const uint32_t largest = std::max(std::max(width, height), depth);
            uint32_t maximum = 1;
            while ((largest >> (maximum - 1)) > 1) ++maximum;

            if (requested == kFullMipChain) return maximum;
            return std::min(requested, maximum);
        }

        /// @brief The view type a shader sees: array-ness is part of it, not a separate flag.
        VkImageViewType toVkImageViewType(ImageType type, uint32_t arrayLayers)
        {
            switch (type) {
                case ImageType::Image1D:
                    return arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
                case ImageType::Image3D:
                    // A 3D image is never an array; its slices are addressed by the third
                    // texture coordinate rather than by a layer index.
                    return VK_IMAGE_VIEW_TYPE_3D;
                case ImageType::CubeMap:
                    return VK_IMAGE_VIEW_TYPE_CUBE;
                case ImageType::Image2D:
                default:
                    return arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
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

    VulkanImage::VulkanImage(VulkanDevice* device, const ImageDesc& desc, const void* initialData)
        : m_data(std::make_unique<VulkanImageNativeData>())
    {
        if (!device) throw std::runtime_error("VulkanImage: null device");
        if (desc.width == 0 || desc.height == 0) throw std::runtime_error("VulkanImage: zero sized image");

        m_data->device = device;
        m_data->owning = true;
        m_data->restingLayout = RestingLayoutFor(desc.usage, /*isSwapChainImage=*/false);
        m_data->format = desc.format;
        m_data->width = desc.width;
        m_data->height = desc.height;
        m_data->usage = desc.usage;
        m_data->type = desc.type;
        m_data->debugName = desc.debugName;
        m_data->sampleCount = desc.sampleCount;

        // A cubemap is six layers by definition; anything else takes what it was given.
        m_data->arrayLayers = (desc.type == ImageType::CubeMap) ? 6 : std::max(1u, desc.arrayLayers);
        m_data->depth = (desc.type == ImageType::Image3D) ? std::max(1u, desc.depth) : 1;

        if (desc.type == ImageType::CubeMap && desc.arrayLayers != 1 && desc.arrayLayers != 6) {
            throw std::runtime_error("VulkanImage: a cubemap has exactly 6 array layers");
        }
        if (m_data->depth > 1 && m_data->arrayLayers > 1) {
            throw std::runtime_error("VulkanImage: arrays of 3D images are not supported");
        }

        if (isCompressedFormat(desc.format)) {
            // All three restrictions are inherent to block compression rather than to this
            // wrapper: blocks cannot be written by the rasteriser, resolved, or downsampled by a
            // blit. A compressed texture arrives fully authored, mip chain and all.
            if (hasFlag(desc.usage, ImageUsage::ColorTarget) ||
                hasFlag(desc.usage, ImageUsage::DepthStencil) ||
                hasFlag(desc.usage, ImageUsage::Storage)) {
                throw std::runtime_error(
                    "VulkanImage: a block-compressed image can only be sampled, not rendered "
                    "into or written by a shader");
            }
            if (desc.sampleCount != SampleCount::One) {
                throw std::runtime_error("VulkanImage: a block-compressed image cannot be multisampled");
            }
        }

        // Mip chains on a volume shrink in three dimensions, so the largest dimension that
        // decides the chain length includes depth.
        m_data->mipLevels = resolveMipLevels(desc.mipLevels, desc.width, desc.height, m_data->depth);

        const bool multisampled = desc.sampleCount != SampleCount::One;
        if (multisampled) {
            // Both restrictions come from the APIs rather than from this wrapper, and hitting
            // either produces a confusing driver error, so say what is wrong up front.
            if (m_data->mipLevels != 1) {
                throw std::runtime_error("VulkanImage: a multisample image cannot have mip levels");
            }
            if (hasFlag(desc.usage, ImageUsage::Sampled)) {
                throw std::runtime_error(
                    "VulkanImage: a multisample image cannot be sampled; render into it and "
                    "resolve into a single-sample image instead");
            }
        }

        VkDevice logicalDevice = device->logicalDevice();
        const VkFormat vkFormat = ToVkFormat(desc.format);

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = toVkImageType(desc.type);
        imageInfo.format = vkFormat;
        imageInfo.extent = { desc.width, desc.height, m_data->depth };
        imageInfo.mipLevels = m_data->mipLevels;
        imageInfo.arrayLayers = m_data->arrayLayers;
        imageInfo.samples = ToVkSampleCount(desc.sampleCount);
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = ToVkImageUsage(desc.usage);
        if (!multisampled) {
            // Uploading needs TRANSFER_DST, and generating the chain blits each level from the
            // one above, so a mipmapped image is also a transfer source. Neither is expressible
            // through ImageUsage, so they are added here rather than making every caller
            // remember them. Multisample images can do neither, so they get neither.
            imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (m_data->mipLevels > 1) {
                imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            }
        }
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        // Every render pass that writes this image either clears it — in which case the contents
        // are discarded and the initial layout is UNDEFINED anyway — or expects it in its resting
        // layout, which a previous pass will have left it in.
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (desc.type == ImageType::CubeMap) {
            imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        }

        VkCheck(vkCreateImage(logicalDevice, &imageInfo, nullptr, &m_data->image), "vkCreateImage");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(logicalDevice, m_data->image, &requirements);

        // Unlike a buffer, a render target has no useful host-visible fallback — a texture the
        // GPU samples every frame has to be in device-local memory to be worth anything. So the
        // capacity check reports the shortfall rather than quietly degrading.
        const MemoryBudget budget = device->queryMemoryBudget();
        if (budget.preciseBudget && budget.availableBytes() < requirements.size) {
            vkDestroyImage(logicalDevice, m_data->image, nullptr);
            m_data->image = VK_NULL_HANDLE;
            throw std::runtime_error(
                "VulkanImage: '" + desc.debugName + "' needs " + std::to_string(requirements.size / 1024) +
                " KiB of device-local memory but only " + std::to_string(budget.availableBytes() / 1024) +
                " KiB is available");
        }

        VkMemoryPropertyFlags chosenFlags = 0;
        const uint32_t memoryTypeIndex = device->selectMemoryType(requirements.memoryTypeBits,
                                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                                  0,
                                                                  chosenFlags);
        // Optimal tiling, so this comes from an image-only pool — see the allocator's note on
        // bufferImageGranularity.
        m_data->allocation = device->allocator().allocate(
            requirements, memoryTypeIndex, VulkanMemoryAllocator::ResourceKind::Optimal);
        VkCheck(vkBindImageMemory(logicalDevice, m_data->image,
                                  m_data->allocation.memory, m_data->allocation.offset),
                "vkBindImageMemory");

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_data->image;
        viewInfo.viewType = toVkImageViewType(desc.type, m_data->arrayLayers);
        viewInfo.format = vkFormat;
        viewInfo.subresourceRange.aspectMask = IsDepthFormat(desc.format)
            ? VK_IMAGE_ASPECT_DEPTH_BIT
            : VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        // The view spans the whole chain so a sampler can select across levels. A render pass
        // attaching this image writes level 0, which is what a single-level view would give.
        viewInfo.subresourceRange.levelCount = m_data->mipLevels;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = imageInfo.arrayLayers;
        VkCheck(vkCreateImageView(logicalDevice, &viewInfo, nullptr, &m_data->imageView), "vkCreateImageView");

        if (initialData) {
            // One layer's worth for arrays and cubemaps, the whole volume for a 3D image.
            update(initialData, levelByteSize(0), 0, 0);
            // A compressed image's lower levels have to be supplied already compressed, so only
            // uncompressed images can have their chain filled in here.
            if (!isCompressedFormat(m_data->format)) {
                generateMipmaps();
            }
        }
    }

    size_t VulkanImage::levelByteSize(uint32_t mipLevel) const
    {
        // imageLevelBytes rounds up to whole blocks, so a compressed level smaller than 4x4
        // still costs one block — which is what the driver expects to be handed.
        return imageLevelBytes(m_data->format,
                               std::max(1u, m_data->width >> mipLevel),
                               std::max(1u, m_data->height >> mipLevel),
                               std::max(1u, m_data->depth >> mipLevel));
    }

    VkImageView VulkanImage::layerView(uint32_t arrayLayer)
    {
        // The whole-image view is what shaders sample; a render pass targeting one layer needs a
        // view of just that layer, since a framebuffer attachment is a view, not an image.
        if (m_data->arrayLayers <= 1) return m_data->imageView;

        if (auto it = m_data->layerViews.find(arrayLayer); it != m_data->layerViews.end()) {
            return it->second;
        }
        if (arrayLayer >= m_data->arrayLayers) {
            throw std::runtime_error("VulkanImage: array layer is out of range");
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_data->image;
        // Always 2D: one layer of an array or one face of a cube is a plain 2D render target.
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = ToVkFormat(m_data->format);
        viewInfo.subresourceRange.aspectMask = IsDepthFormat(m_data->format)
            ? VK_IMAGE_ASPECT_DEPTH_BIT
            : VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = arrayLayer;
        viewInfo.subresourceRange.layerCount = 1;

        VkImageView view = VK_NULL_HANDLE;
        VkCheck(vkCreateImageView(m_data->device->logicalDevice(), &viewInfo, nullptr, &view),
                "vkCreateImageView (layer)");
        m_data->layerViews.emplace(arrayLayer, view);
        return view;
    }

    void VulkanImage::update(const void* data, size_t dataSize, uint32_t mipLevel, uint32_t arrayLayer)
    {
        if (!data || dataSize == 0) return;
        if (!m_data->owning) {
            throw std::runtime_error("GImage::update: a swapchain image cannot be written from the CPU");
        }
        if (mipLevel >= m_data->mipLevels) {
            throw std::runtime_error("GImage::update: mip level is out of range");
        }
        if (arrayLayer >= m_data->arrayLayers) {
            throw std::runtime_error("GImage::update: array layer is out of range");
        }

        const size_t expected = levelByteSize(mipLevel);
        if (dataSize != expected) {
            throw std::runtime_error(
                "GImage::update: expected " + std::to_string(expected) + " bytes for mip level " +
                std::to_string(mipLevel) + " but received " + std::to_string(dataSize));
        }

        m_data->device->uploadToImage(m_data->image,
                                      std::max(1u, m_data->width >> mipLevel),
                                      std::max(1u, m_data->height >> mipLevel),
                                      std::max(1u, m_data->depth >> mipLevel),
                                      mipLevel, arrayLayer,
                                      data, dataSize, m_data->restingLayout);
    }

    void VulkanImage::readback(void* destination, size_t destinationSize,
                               uint32_t mipLevel, uint32_t arrayLayer)
    {
        if (!destination || destinationSize == 0) return;
        if (!m_data->owning) {
            throw std::runtime_error("GImage::readback: a swapchain image cannot be read back");
        }
        if (m_data->sampleCount != SampleCount::One) {
            throw std::runtime_error(
                "GImage::readback: a multisample image cannot be read back; resolve it first");
        }
        if (!hasFlag(m_data->usage, ImageUsage::TransferSrc)) {
            throw std::runtime_error(
                "GImage::readback: image was not created with ImageUsage::TransferSrc");
        }
        if (mipLevel >= m_data->mipLevels) {
            throw std::runtime_error("GImage::readback: mip level is out of range");
        }
        if (arrayLayer >= m_data->arrayLayers) {
            throw std::runtime_error("GImage::readback: array layer is out of range");
        }

        const size_t expected = levelByteSize(mipLevel);
        if (destinationSize != expected) {
            throw std::runtime_error(
                "GImage::readback: expected " + std::to_string(expected) + " bytes for mip level " +
                std::to_string(mipLevel) + " but the destination is " + std::to_string(destinationSize));
        }

        m_data->device->readbackFromImage(m_data->image,
                                          std::max(1u, m_data->width >> mipLevel),
                                          std::max(1u, m_data->height >> mipLevel),
                                          std::max(1u, m_data->depth >> mipLevel),
                                          mipLevel, arrayLayer,
                                          destination, destinationSize, m_data->restingLayout);
    }

    void VulkanImage::generateMipmaps()
    {
        if (m_data->mipLevels <= 1) return;
        if (isCompressedFormat(m_data->format)) {
            // vkCmdBlitImage cannot read or write compressed blocks. Compressing on the GPU is a
            // whole subsystem; the expectation is that the chain was compressed offline and
            // uploaded level by level with update().
            throw std::runtime_error(
                "GImage::generateMipmaps: a block-compressed image's mip levels must be supplied "
                "already compressed, one update() per level");
        }
        m_data->device->generateMipmaps(m_data->image, ToVkFormat(m_data->format),
                                        m_data->width, m_data->height, m_data->depth,
                                        m_data->mipLevels, m_data->arrayLayers,
                                        m_data->restingLayout);
    }

    VulkanImage::~VulkanImage()
    {
        if (!m_data->owning) return;

        VkDevice logicalDevice = m_data->device->logicalDevice();
        // See VulkanBuffer's destructor: Vulkan will not keep this alive for an in-flight frame.
        vkDeviceWaitIdle(logicalDevice);

        // Framebuffers built from any of these views must go first — they hold them by handle.
        m_data->device->invalidateFramebuffersUsing(m_data->imageView);
        for (auto& [layer, view] : m_data->layerViews) {
            m_data->device->invalidateFramebuffersUsing(view);
            vkDestroyImageView(logicalDevice, view, nullptr);
        }
        m_data->layerViews.clear();

        if (m_data->imageView) vkDestroyImageView(logicalDevice, m_data->imageView, nullptr);
        if (m_data->image) vkDestroyImage(logicalDevice, m_data->image, nullptr);
        if (m_data->allocation.isValid()) m_data->device->allocator().free(m_data->allocation);
    }

    uint32_t VulkanImage::width() const { return m_data->width; }
    uint32_t VulkanImage::height() const { return m_data->height; }
    uint32_t VulkanImage::depth() const { return m_data->depth; }
    uint32_t VulkanImage::arrayLayers() const { return m_data->arrayLayers; }
    uint32_t VulkanImage::mipLevels() const { return m_data->mipLevels; }
    ImageFormat VulkanImage::format() const { return m_data->format; }
    ImageType VulkanImage::type() const { return m_data->type; }
    ImageUsage VulkanImage::usage() const { return m_data->usage; }
    SampleCount VulkanImage::sampleCount() const { return m_data->sampleCount; }

    MemoryLocation VulkanImage::memoryLocation() const
    {
        // Both cases are device-local: images this class allocates ask for DEVICE_LOCAL memory,
        // and swapchain images are owned by the presentation engine, which keeps them in VRAM.
        return MemoryLocation::DeviceLocal;
    }

    void* VulkanImage::nativeHandle() const { return (void*)&m_data->image; }

    void* VulkanImage::nativeDrawableHandle() const { return (void*)m_data->swapChain; }

    const std::string& VulkanImage::debugName() const { return m_data->debugName; }

    void VulkanImage::setDebugName(const std::string& name) { m_data->debugName = name; }

    VkImageView VulkanImage::imageView() const { return m_data->imageView; }

    uint32_t VulkanImage::imageIndex() const { return m_data->imageIndex; }

    VulkanSwapChain* VulkanImage::swapChain() const { return m_data->swapChain; }

    VkImageLayout VulkanImage::restingLayout() const { return m_data->restingLayout; }

} // namespace dmrender
