#import "MetalSwapChain.hpp"
#import "MetalCommandQueues.hpp"
#import "MetalOutSurface.hpp"
#import "MetalUtilsCpp.hpp"
#import "MetalImage.hpp"
#import <QuartzCore/CAMetalLayer.h>
#import <Cocoa/Cocoa.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace dmrender {
 struct MetalSwapChainNativeData{
     CAMetalLayer* m_layer{};
     id<MTLCommandBuffer> m_commandBuffer{};
 };
    MetalSwapChain::MetalSwapChain(std::shared_ptr<Device> device, std::shared_ptr<CommandQueue> cmdLists,
                                   std::shared_ptr<Surface> outSurf, const size_t width, const size_t height)
            : SwapChain(std::move(device), std::move(cmdLists), std::move(outSurf), width, height), m_data(std::make_unique<MetalSwapChainNativeData>()) {

        auto* mSurface = (MetalOutSurface*)m_surface.get();
        m_data->m_layer = [CAMetalLayer layer];
        auto content = (NSView*) mSurface->nativeHandle();
        m_data->m_layer.device =  (id<MTLDevice>) m_device->nativeHandle();
        m_data->m_layer.pixelFormat = ToMTLPixelFormat(m_surface->getFormat());
        content.layer = m_data->m_layer;
        m_data->m_layer.drawableSize = CGSizeMake(m_width, m_height);
        m_data->m_commandBuffer = (__bridge id<MTLCommandBuffer>) m_commandQueue->nativeHandle();
    }

    std::shared_ptr<GImage>  MetalSwapChain::acquireNextImage() {
        // Opens the frame here rather than leaving it to the command buffer's constructor,
        // because acquiring is the first thing a frame does and everything a frame uploads —
        // uniforms, indirect commands — happens between this call and the first command buffer.
        // Those writes land in the current frame's buffer regions, so the wait that guarantees
        // the GPU has finished with those regions has to come first. Waiting before taking a
        // drawable also means the frame is not sitting on one while it blocks.
        if (auto* queue = static_cast<MetalCommandQueues*>(m_commandQueue.get())) {
            queue->beginFrame();
        }

        id<CAMetalDrawable> drawable = [m_data->m_layer nextDrawable];

        if (!drawable) {
            // The frame was opened but nothing will be submitted into it, so the permit taken
            // above has to go back — otherwise every minimised frame leaks one and the pipeline
            // deadlocks after kFramesInFlight of them.
            if (auto* queue = static_cast<MetalCommandQueues*>(m_commandQueue.get())) {
                queue->abandonFrame();
            }
            return nullptr; // the window may be minimised
        }
        auto metalImg = std::make_shared<MetalImage>(
                drawable, m_surface->getFormat(), ImageUsage::ColorTarget, ImageType::Image2D);
        return metalImg;
    }
    std::uint32_t MetalSwapChain::width() const {
        return m_width;
    }
    std::uint32_t MetalSwapChain::height() const {
        return m_height;
    }

    void MetalSwapChain::recreate(uint32_t width, uint32_t height) {
        if (m_data->m_layer) {
            m_data->m_layer.drawableSize = CGSizeMake(width, height);
            m_width = width;
            m_height = height;
        }
    }

    MetalSwapChain::~MetalSwapChain(){
        if (m_data->m_layer) {
            m_data->m_layer = nullptr;
            m_data->m_commandBuffer = nil;
        }
    }
}