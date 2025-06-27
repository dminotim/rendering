#import <Metal/Metal.h>
#import "MetalShaderFunction.hpp"

namespace dmrender {

    struct MetalShaderFunctionNativeData
    {
        id<MTLFunction> m_func;
    };

    MetalShaderFunction::MetalShaderFunction(const std::shared_ptr<Device>& device,
                                             const std::string& source,
                                             const std::string& entryPoint,
                                             const std::string& label)
            : m_entryPoint(entryPoint), m_data(std::make_unique<MetalShaderFunctionNativeData>())
    {
        auto mtlDevice = (id<MTLDevice>) device->nativeHandle();

        NSError* error = nil;
        NSString* src = [NSString stringWithUTF8String:source.c_str()];
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];

        id<MTLLibrary> library = [mtlDevice newLibraryWithSource:src options:options error:&error];
        [options release];

        if (!library) {
            NSLog(@" Shader compilation error: %@", error.localizedDescription);
            throw std::runtime_error("Failed to compile Metal shader");
        }

        NSString* entry = [NSString stringWithUTF8String:entryPoint.c_str()];
        m_data->m_func = [[library newFunctionWithName:entry] retain];
        [library release];

        if (!m_data->m_func) {
            throw std::runtime_error("Failed to extract function from compiled Metal library");
        }

        if (!label.empty()) {
            m_data->m_func.label = [NSString stringWithUTF8String:label.c_str()];
        }

    }

    MetalShaderFunction::~MetalShaderFunction() {
        if (m_data->m_func) {
            [m_data->m_func release];
            m_data->m_func = nullptr;
        }
    }

    void* MetalShaderFunction::nativeHandle() const {
        return (__bridge void*)m_data->m_func;
    }

    const char* MetalShaderFunction::entryPoint() const {
        return m_entryPoint.c_str();
    }

}