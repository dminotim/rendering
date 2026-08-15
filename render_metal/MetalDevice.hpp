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
            ImageType type,
            ImageFormat format,
            uint32_t width,
            uint32_t height,
            ImageUsage usage,
            const std::string& debugName
    ) override;

    std::shared_ptr<GSampler> createSampler(
            const SamplerDesc& desc,
            const std::string& debugName
    ) override;

    MemoryBudget queryMemoryBudget() const override;

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

    void* nativeHandle() const override;

    MetalDevice(DeviceId id, void* nativeDevice);
private:
    /// @brief Pointer to implementation (PIMPL) to hide Metal-specific details.
    std::unique_ptr<MetalDeviceNativeData> m_data;
    std::set<DeviceExtension> m_activatedInstanceExtensions;
};

}
#endif //RENDERING_METALDEVICE_HPP
