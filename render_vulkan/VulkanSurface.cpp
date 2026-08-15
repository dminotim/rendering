#include "VulkanSurface.hpp"
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <render_vulkan/VulkanSingleton.hpp>
#include <stdexcept>
namespace dmrender
{
	struct VulkanSurfaceNativeData
	{
		VkSurfaceKHR surface;
	};
}

dmrender::VulkanSurface::VulkanSurface(GLFWwindow* window, ImageFormat imageFormat_) 
	: Surface(window, imageFormat_), m_data(std::make_unique<VulkanSurfaceNativeData>())
{
	VkSurfaceKHR& surface = m_data->surface;
	if (glfwCreateWindowSurface(VulkanSingleton::getInstance().nativeHandle(), window, nullptr, &surface) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create window surface!");
	}
}

dmrender::VulkanSurface::~VulkanSurface()
{
	if (m_data->surface)
		vkDestroySurfaceKHR(VulkanSingleton::getInstance().nativeHandle(), m_data->surface, nullptr);
}

void* dmrender::VulkanSurface::nativeHandle() const
{
	return  static_cast<void*>(&(m_data->surface));
}
