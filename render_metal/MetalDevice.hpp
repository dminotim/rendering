//
// Created by Artem Avdoshkin on 11.06.2025.
//

#ifndef RENDERING_METALDEVICE_HPP
#define RENDERING_METALDEVICE_HPP
#include "Device.hpp"
#include "Surface.hpp"
#include <set>

namespace dmrender {

struct MetalDeviceNativeData;

class MetalDevice : public Device
{
public:

    static std::shared_ptr<Device>  createDefaultDevice(const std::shared_ptr<Surface>& surface);
    static std::shared_ptr<Device>  createDeviceById(const DeviceId& id);
    static std::vector<DeviceId> enumerateAvailableDevices();

    ~MetalDevice() override;

    bool activateExtension(DeviceExtension ext) override;
    bool isExtensionAvailable(DeviceExtension ext) const override;
   
    std::shared_ptr<GBuffer> createBuffer(
            BufferType type,
            BufferUsage usage,
            size_t size,
            const void* initialData,
            const std::string& debugName
    ) override;

    std::shared_ptr<GImage> createImage(
            const ImageDesc& desc,
            const void* initialData
    ) override;

    std::shared_ptr<GSampler> createSampler(
            const SamplerDesc& desc,
            const std::string& debugName
    ) override;

    MemoryBudget queryMemoryBudget() const override;

    SampleCount maxSupportedSampleCount() const override;

    /**
     * @brief Copies CPU data into a private-storage MTLBuffer through a staging buffer.
     *
     * MTLStorageModePrivate memory has no `contents` pointer, so the bytes are written into a
     * shared-storage staging buffer and copied on the GPU with a blit encoder. The command
     * buffer is waited on before returning, making this synchronous — appropriate at resource
     * creation time and deliberately unattractive per frame.
     *
     * @param destination The private MTLBuffer to write into, as an id<MTLBuffer>.
     * @param destinationOffset Byte offset within @p destination.
     * @param data The bytes to upload.
     * @param size How many bytes to upload.
     */
    void uploadToPrivateBuffer(void* destination,
                               size_t destinationOffset,
                               const void* data,
                               size_t size) const;

    /**
     * @brief Copies tightly packed pixels into one mip level of a private-storage MTLTexture.
     *
     * Same reasoning as uploadToPrivateBuffer: Private textures have no CPU-visible contents, so
     * the pixels go through a shared staging buffer and a blit. Synchronous.
     *
     * @param destination The private MTLTexture to write into, as an id<MTLTexture>.
     * @param width Width of this mip level in pixels.
     * @param height Height of this mip level in pixels.
     * @param mipLevel Which level to write.
     * @param data Tightly packed pixels.
     * @param size Size of @p data in bytes.
     * @param bytesPerRow Distance between rows. For a compressed format this is a row of
     *                    *blocks*, covering four texel rows, so it cannot be derived from
     *                    size and height alone.
     * @param bytesPerImage Distance between depth slices; equals @p size for a 2D image.
     */
    void uploadToPrivateTexture(void* destination,
                                uint32_t width,
                                uint32_t height,
                                uint32_t depth,
                                uint32_t mipLevel,
                                uint32_t arrayLayer,
                                const void* data,
                                size_t size,
                                size_t bytesPerRow,
                                size_t bytesPerImage) const;

    /**
     * @brief Fills levels 1..n-1 of a texture from level 0.
     *
     * Metal exposes this directly on the blit encoder, so unlike the Vulkan path there is no
     * per-level blit loop or layout dance to write out.
     *
     * @param texture The MTLTexture to fill, as an id<MTLTexture>.
     */
    void generateMipmapsForTexture(void* texture) const;

    /**
     * @brief Copies one mip level of a private-storage texture back into CPU memory.
     * @param source The MTLTexture to read, as an id<MTLTexture>.
     * @param width Width of this mip level in pixels.
     * @param height Height of this mip level in pixels.
     * @param mipLevel Which level to read.
     * @param destination Buffer to fill with tightly packed pixels.
     * @param size Number of bytes to read.
     */
    void readbackFromPrivateTexture(void* source,
                                    uint32_t width,
                                    uint32_t height,
                                    uint32_t depth,
                                    uint32_t mipLevel,
                                    uint32_t arrayLayer,
                                    void* destination,
                                    size_t size,
                                    size_t bytesPerRow,
                                    size_t bytesPerImage) const;

    /// @brief Copies a range of a private-storage buffer back into CPU memory.
    void readbackFromPrivateBuffer(void* source,
                                   size_t sourceOffset,
                                   void* destination,
                                   size_t size) const;

private:
    /// @brief Creates the transfer queue on first use and grows the staging buffer as needed.
    void ensureTransferResources(size_t stagingSize) const;

public:

    void* nativeHandle() const override;

    MetalDevice(DeviceId id, void* nativeDevice);
private:
    /// @brief Pointer to implementation (PIMPL) to hide Metal-specific details.
    std::unique_ptr<MetalDeviceNativeData> m_data;
    std::set<DeviceExtension> m_activatedInstanceExtensions;
};

}
#endif //RENDERING_METALDEVICE_HPP
