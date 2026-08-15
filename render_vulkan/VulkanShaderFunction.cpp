#include "VulkanShaderFunction.hpp"

#include <fstream>
#include <stdexcept>
#include <vector>

#include <render_vulkan/VulkanDevice.hpp>
#include <render_vulkan/VulkanUtils.hpp>

namespace dmrender {

    struct VulkanShaderFunctionNativeData
    {
        VulkanDevice* device = nullptr;
        VkShaderModule shaderModule = VK_NULL_HANDLE;
        std::string functionName;
        std::string entryPoint = "main";
    };

    namespace {

        /// Turns `<dir>/PlaneShader[.anything]` + `plane_vertex_shader` into
        /// `<dir>/PlaneShader.plane_vertex_shader.spv`.
        std::filesystem::path resolveSpirvPath(const std::filesystem::path& pathToShaderFile,
                                               const std::string& functionName)
        {
            std::filesystem::path resolved = pathToShaderFile;
            resolved.replace_filename(pathToShaderFile.stem().string() + "." + functionName + ".spv");
            return resolved;
        }

        std::vector<char> readFile(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::ate | std::ios::binary);
            if (!file.is_open()) {
                throw std::runtime_error("VulkanShaderFunction: cannot open SPIR-V file " + path.string());
            }
            const auto size = static_cast<std::streamsize>(file.tellg());
            std::vector<char> buffer(static_cast<size_t>(size));
            file.seekg(0);
            file.read(buffer.data(), size);
            return buffer;
        }

    } // namespace

    VulkanShaderFunction::VulkanShaderFunction(const std::shared_ptr<Device>& device,
                                               const std::filesystem::path& pathToShaderFile,
                                               const std::string& functionName)
        : m_data(std::make_unique<VulkanShaderFunctionNativeData>())
    {
        m_data->device = static_cast<VulkanDevice*>(device.get());
        m_data->functionName = functionName;

        const std::filesystem::path spirvPath = resolveSpirvPath(pathToShaderFile, functionName);
        const std::vector<char> code = readFile(spirvPath);
        if (code.empty() || (code.size() % 4) != 0) {
            throw std::runtime_error("VulkanShaderFunction: " + spirvPath.string() + " is not valid SPIR-V");
        }

        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

        VkCheck(vkCreateShaderModule(m_data->device->logicalDevice(), &createInfo, nullptr, &m_data->shaderModule),
                "vkCreateShaderModule");
    }

    VulkanShaderFunction::~VulkanShaderFunction()
    {
        if (m_data->shaderModule) {
            vkDestroyShaderModule(m_data->device->logicalDevice(), m_data->shaderModule, nullptr);
            m_data->shaderModule = VK_NULL_HANDLE;
        }
    }

    void* VulkanShaderFunction::nativeHandle() const { return (void*)&m_data->shaderModule; }

    const char* VulkanShaderFunction::entryPoint() const { return m_data->entryPoint.c_str(); }

    VkShaderModule VulkanShaderFunction::shaderModule() const { return m_data->shaderModule; }

    const std::string& VulkanShaderFunction::functionName() const { return m_data->functionName; }

} // namespace dmrender
