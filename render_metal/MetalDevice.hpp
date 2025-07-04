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
    // Статический метод для создания экземпляра MetalDevice
    // Можно выбрать устройство по умолчанию или специфическое
    static std::shared_ptr<Device>  createDefaultDevice(const std::shared_ptr<Surface>& surface);
    static std::shared_ptr<Device>  createDeviceById(const DeviceId& id); // Попытаться найти по имени/ID
    static std::vector<DeviceId> enumerateAvailableDevices();

    ~MetalDevice() override;

    // Реализация виртуальных методов
    bool activateExtension(DeviceExtension ext) override;
    bool isExtensionAvailable(DeviceExtension ext) const override;
    // В Metal это будет похоже
    std::shared_ptr<GBuffer> createBuffer(
            BufferType type,
            BufferUsage usage,
            size_t size,
            const void* initialData,
            const std::string& debugName
    ) override;

    // В Metal это, вероятно, будет no-op

    // Специфичный для Metal метод для получения нативного MTLDevice
    // Это не часть абстрактного интерфейса, но полезно для работы с Metal
    void* nativeHandle() const override;

    MetalDevice(DeviceId id, void* nativeDevice);
private:
    /// @brief Pointer to implementation (PIMPL) to hide Metal-specific details.
    std::unique_ptr<MetalDeviceNativeData> m_data;
    std::set<DeviceExtension> m_activatedInstanceExtensions;
};

}
#endif //RENDERING_METALDEVICE_HPP
