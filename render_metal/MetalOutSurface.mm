

#import "MetalOutSurface.hpp"
#import "MetalUtilsCpp.hpp"
#import <Metal/Metal.h>
#import <Cocoa/Cocoa.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#import <QuartzCore/CAMetalLayer.h>

namespace dmrender {

    struct MetalOutSurfaceNativeData
    {
        NSView* m_nsview;
        NSWindow* m_nswin;
    };
    MetalOutSurface::MetalOutSurface(GLFWwindow *window, ImageFormat imageFormat_)
    : Surface(window, imageFormat_), m_data(std::make_unique<MetalOutSurfaceNativeData>()) {
        m_data->m_nswin = glfwGetCocoaWindow(window);
        m_data->m_nsview = (m_data->m_nswin).contentView;
        m_data->m_nsview.wantsLayer = YES;
    }

    MetalOutSurface::~MetalOutSurface() {
    }

    void *MetalOutSurface::nativeHandle() const {
        return (void*)m_data->m_nsview;
    }

}