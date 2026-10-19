#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef ZVRAM_GRAPHICS_SDL2
#include <SDL.h>
#include <SDL_vulkan.h>
#endif

#ifndef ZVRAM_GRAPHICS_VERT_PATH
#define ZVRAM_GRAPHICS_VERT_PATH "graphics_check.vert.spv"
#endif
#ifndef ZVRAM_GRAPHICS_FRAG_PATH
#define ZVRAM_GRAPHICS_FRAG_PATH "graphics_check.frag.spv"
#endif

namespace {
constexpr VkDeviceSize BufferBytes = 32ull * 1024 * 1024;
constexpr std::uint32_t Width = 16, Height = 16;
constexpr auto ColdTimeout = std::chrono::seconds(3);

void check(VkResult r, const char* op) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(op) + ": " + std::to_string(r));
}
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::uint32_t mix(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; return x ^ (x >> 16);
}
std::vector<std::uint32_t> makeInput() {
    std::vector<std::uint32_t> v(static_cast<std::size_t>(BufferBytes / 4), 0x3f000000u);
    const std::array<float, 6> xy{-1.f,-1.f, 3.f,-1.f, -1.f,3.f};
    std::memcpy(v.data(), xy.data(), sizeof(xy));
    for (std::size_t i = 32; i < v.size(); i += 32) v[i] = mix(static_cast<std::uint32_t>(i));
    return v;
}
std::vector<std::uint32_t> readSpv(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    require(bool(f), "cannot open shader SPIR-V");
    const auto n = f.tellg();
    require(n > 0 && (n % 4) == 0, "invalid shader SPIR-V size");
    std::vector<std::uint32_t> code(static_cast<std::size_t>(n) / 4);
    f.seekg(0); f.read(reinterpret_cast<char*>(code.data()), n);
    require(bool(f), "cannot read shader SPIR-V");
    return code;
}

struct Stats {
    std::uint32_t structSize{}, version{};
    std::uint64_t coldLogicalBytes{}, coldStoredBytes{}, coldBudgetBytes{}, residentBytes{};
    std::uint64_t freezes{}, restores{}, failures{};
    std::int32_t lastError{};
};
using GetStats = VkResult (VKAPI_PTR *)(VkDevice, Stats*);
std::atomic<unsigned> validationErrors{};
VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validationErrors;
        std::cerr << "Vulkan validation error: " << (data && data->pMessage ? data->pMessage : "(none)") << '\n';
    }
    return VK_FALSE;
}

struct Fixture {
    VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{};
    VkDebugUtilsMessengerEXT messenger{};
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger{};
    VkCommandPool pool{}; VkFence fence{}; VkBuffer data{}, upload{}, readback{};
    VkDeviceMemory dataMem{}, uploadMem{}, readbackMem{};
    VkImage image{}; VkDeviceMemory imageMem{}; VkImageView view{};
    VkRenderPass renderPass{}; VkFramebuffer framebuffer{};
    VkDescriptorSetLayout setLayout{}; VkDescriptorPool descriptorPool{}; VkDescriptorSet set{};
    VkPipelineLayout pipelineLayout{}; VkPipeline pipeline{};
    VkSurfaceKHR surface{}; VkSwapchainKHR swapchain{}; VkFormat colorFormat{VK_FORMAT_R8G8B8A8_UNORM};
    VkExtent2D extent{Width,Height}; std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews; std::vector<VkFramebuffer> swapFramebuffers;
    std::vector<VkSemaphore> acquireSemaphores, renderSemaphores;
    std::vector<std::uint64_t> submitCallMicros, fenceWaitCallMicros;
    VkDeviceSize descriptorRange{BufferBytes};
    bool presenting{}, nativeAllocation{}, presentIdMetadata{}, presentRegionsMetadata{};
    bool unsupported{};
    std::string unsupportedReason;
    std::uint64_t nextPresentId{1};
    bool abandon{}, validationOn{}; GetStats getStats{};

    ~Fixture() {
        if (abandon) return;
        // The submit fence does not cover consumption of the presentation wait semaphore.
        // The test runner supplies a process timeout around this unbounded Vulkan API.
        if (presenting && queue && vkQueueWaitIdle(queue)!=VK_SUCCESS) {
            ++validationErrors;
            std::cerr << "FAIL: graphics presentation cleanup could not retire queue\n";
            return;
        }
        if (device && fence) vkDestroyFence(device, fence, nullptr);
        if (device && pool) vkDestroyCommandPool(device, pool, nullptr);
        for (auto s:acquireSemaphores) if(device&&s) vkDestroySemaphore(device,s,nullptr);
        for (auto s:renderSemaphores) if(device&&s) vkDestroySemaphore(device,s,nullptr);
        for (auto f:swapFramebuffers) if(device&&f) vkDestroyFramebuffer(device,f,nullptr);
        for (auto v:swapViews) if(device&&v) vkDestroyImageView(device,v,nullptr);
        if (device && swapchain) vkDestroySwapchainKHR(device,swapchain,nullptr);
        if (device && pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (device && pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (device && descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        if (device && setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        if (device && framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
        if (device && renderPass) vkDestroyRenderPass(device, renderPass, nullptr);
        if (device && view) vkDestroyImageView(device, view, nullptr);
        if (device && image) vkDestroyImage(device, image, nullptr);
        if (device && imageMem) vkFreeMemory(device, imageMem, nullptr);
        if (device && data) vkDestroyBuffer(device, data, nullptr);
        if (device && upload) vkDestroyBuffer(device, upload, nullptr);
        if (device && readback) vkDestroyBuffer(device, readback, nullptr);
        if (device && dataMem) vkFreeMemory(device, dataMem, nullptr);
        if (device && uploadMem) vkFreeMemory(device, uploadMem, nullptr);
        if (device && readbackMem) vkFreeMemory(device, readbackMem, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance && messenger && destroyMessenger) destroyMessenger(instance,messenger,nullptr);
        if (instance && surface) vkDestroySurfaceKHR(instance,surface,nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    std::uint32_t memoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties m{}; vkGetPhysicalDeviceMemoryProperties(physical, &m);
        if (getStats && !nativeAllocation && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            const auto i=m.memoryTypeCount-1;
            if ((bits & (1u<<i)) && m.memoryTypes[i].propertyFlags==VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) return i;
        }
        for (std::uint32_t i=0;i<m.memoryTypeCount;++i) {
            if (getStats && nativeAllocation && i==m.memoryTypeCount-1) continue;
            const auto props=m.memoryTypes[i].propertyFlags;
            if ((bits & (1u<<i)) && (props & flags)==flags &&
                (!(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) || !(props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) return i;
        }
        if (!getStats && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            for (std::uint32_t i=0;i<m.memoryTypeCount;++i)
                if ((bits & (1u<<i)) && (m.memoryTypes[i].propertyFlags & flags)==flags) return i;
        throw std::runtime_error("required memory type unavailable");
    }
    void allocBuffer(VkBuffer b, VkMemoryPropertyFlags flags, VkDeviceMemory& mem) {
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device,b,&req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size;
        ai.memoryTypeIndex=memoryType(req.memoryTypeBits,flags);
        check(vkAllocateMemory(device,&ai,nullptr,&mem),"allocate buffer memory");
        check(vkBindBufferMemory(device,b,mem,0),"bind buffer memory");
    }
    VkBuffer makeBuffer(VkDeviceSize size,VkBufferUsageFlags usage,VkMemoryPropertyFlags flags,VkDeviceMemory& mem) {
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size=size; ci.usage=usage; ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer b{}; check(vkCreateBuffer(device,&ci,nullptr,&b),"create buffer");
        try { allocBuffer(b,flags,mem); }
        catch(...) {
            vkDestroyBuffer(device,b,nullptr);
            if(mem) { vkFreeMemory(device,mem,nullptr); mem=VK_NULL_HANDLE; }
            throw;
        }
        return b;
    }
    VkCommandBuffer begin() {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool=pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1;
        VkCommandBuffer cmd{}; check(vkAllocateCommandBuffers(device,&ai,&cmd),"allocate command buffer");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cmd,&bi),"begin command buffer"); return cmd;
    }
    void submit(VkCommandBuffer cmd, VkSemaphore waitSemaphore=VK_NULL_HANDLE,
                VkSemaphore signalSemaphore=VK_NULL_HANDLE, bool sampleCalls=false) {
        check(vkEndCommandBuffer(cmd),"end command buffer"); check(vkResetFences(device,1,&fence),"reset fence");
        const VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        if(waitSemaphore) { si.waitSemaphoreCount=1; si.pWaitSemaphores=&waitSemaphore; si.pWaitDstStageMask=&waitStage; }
        si.commandBufferCount=1; si.pCommandBuffers=&cmd;
        if(signalSemaphore) { si.signalSemaphoreCount=1; si.pSignalSemaphores=&signalSemaphore; }
        const auto submitStart=std::chrono::steady_clock::now();
        const auto submitted=vkQueueSubmit(queue,1,&si,fence);
        if(sampleCalls) submitCallMicros.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-submitStart).count()));
        if (submitted==VK_ERROR_DEVICE_LOST) abandon=true;
        check(submitted,"submit graphics fixture");
        const auto waitStart=std::chrono::steady_clock::now();
        const auto r=vkWaitForFences(device,1,&fence,VK_TRUE,3'000'000'000ull);
        if(sampleCalls) fenceWaitCallMicros.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-waitStart).count()));
        if (r==VK_TIMEOUT || r==VK_ERROR_DEVICE_LOST) abandon=true;
        check(r,"wait for graphics fixture");
        vkFreeCommandBuffers(device,pool,1,&cmd);
    }
    Stats stats() const {
        Stats s{}; s.structSize=sizeof(s); s.version=1; check(getStats(device,&s),"query zVram stats");
        require(s.version==1 && s.structSize==sizeof(s),"snapshot stats ABI mismatch"); return s;
    }
    Stats waitCold(std::uint64_t oldFreezes) const {
        const auto until=std::chrono::steady_clock::now()+ColdTimeout;
        do {
            auto s=stats();
            if (s.coldLogicalBytes>=BufferBytes && s.coldStoredBytes && s.coldStoredBytes<s.coldLogicalBytes &&
                s.residentBytes==0 && s.freezes>oldFreezes) return s;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } while(std::chrono::steady_clock::now()<until);
        throw std::runtime_error("32 MiB buffer did not return to cold state before timeout");
    }
    void init(bool native, bool requestPresentId=false, bool requestPresentRegions=false
#ifdef ZVRAM_GRAPHICS_SDL2
              , bool present=false, SDL_Window* window=nullptr
#endif
              ) {
        presenting=false;
#ifdef ZVRAM_GRAPHICS_SDL2
        presenting=present;
#else
        (void)native;
#endif
        presentIdMetadata=requestPresentId;
        presentRegionsMetadata=requestPresentRegions;
        require((!requestPresentId&&!requestPresentRegions)||presenting,
                "presentation metadata requires SDL presentation");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_1;
        std::uint32_t layerCount=0, extCount=0;
        check(vkEnumerateInstanceLayerProperties(&layerCount,nullptr),"enumerate layers");
        std::vector<VkLayerProperties> layers(layerCount); check(vkEnumerateInstanceLayerProperties(&layerCount,layers.data()),"enumerate layers");
        const bool validation=std::any_of(layers.begin(),layers.end(),[](const auto& x){return std::strcmp(x.layerName,"VK_LAYER_KHRONOS_validation")==0;}); validationOn=validation;
        check(vkEnumerateInstanceExtensionProperties(nullptr,&extCount,nullptr),"enumerate extensions");
        std::vector<VkExtensionProperties> exts(extCount); check(vkEnumerateInstanceExtensionProperties(nullptr,&extCount,exts.data()),"enumerate extensions");
        const bool debug=std::any_of(exts.begin(),exts.end(),[](const auto& x){return std::strcmp(x.extensionName,VK_EXT_DEBUG_UTILS_EXTENSION_NAME)==0;});
        const char* layerName="VK_LAYER_KHRONOS_validation"; const char* extName=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkDebugUtilsMessengerCreateInfoEXT debugInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debugInfo.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugInfo.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugInfo.pfnUserCallback=validationCallback;
#ifdef ZVRAM_GRAPHICS_SDL2
        std::vector<const char*> enabledExts;
        if (present) {
            require(window,"SDL window required for --present");
            unsigned sdlCount=0; require(SDL_Vulkan_GetInstanceExtensions(window,&sdlCount,nullptr)==SDL_TRUE,"query SDL Vulkan extensions");
            std::vector<const char*> sdlExts(sdlCount); require(SDL_Vulkan_GetInstanceExtensions(window,&sdlCount,sdlExts.data())==SDL_TRUE,"read SDL Vulkan extensions");
            enabledExts=sdlExts;
        }
        if(debug && std::find_if(enabledExts.begin(),enabledExts.end(),[&](const char* e){return std::strcmp(e,extName)==0;})==enabledExts.end()) enabledExts.push_back(extName);
#else
        std::vector<const char*> enabledExts; if(debug) enabledExts.push_back(extName);
#endif
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app;
        ici.enabledLayerCount=validation?1u:0u; ici.ppEnabledLayerNames=validation?&layerName:nullptr;
        ici.enabledExtensionCount=static_cast<std::uint32_t>(enabledExts.size()); ici.ppEnabledExtensionNames=enabledExts.data(); ici.pNext=debug?&debugInfo:nullptr;
        check(vkCreateInstance(&ici,nullptr,&instance),"create instance");
        std::cout<<"graphics-validation="<<(validationOn?"on":"unavailable")<<'\n';
        if(debug) {
            auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
            destroyMessenger=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));
            if(create&&destroyMessenger) check(create(instance,&debugInfo,nullptr,&messenger),"create debug messenger");
        }
#ifdef ZVRAM_GRAPHICS_SDL2
        if(present) require(SDL_Vulkan_CreateSurface(window,instance,&surface)==SDL_TRUE,"create SDL Vulkan surface");
#endif
        std::uint32_t n=0; check(vkEnumeratePhysicalDevices(instance,&n,nullptr),"enumerate devices"); require(n,"no Vulkan device");
        std::vector<VkPhysicalDevice> devices(n); check(vkEnumeratePhysicalDevices(instance,&n,devices.data()),"enumerate devices");
        bool metadataSupportedDevice=false;
        for(auto d:devices) {
#ifdef ZVRAM_GRAPHICS_SDL2
            if(present) {
                std::uint32_t ec=0;
                check(vkEnumerateDeviceExtensionProperties(d,nullptr,&ec,nullptr),"query device extensions");
                std::vector<VkExtensionProperties> ex(ec);
                check(vkEnumerateDeviceExtensionProperties(d,nullptr,&ec,ex.data()),"read device extensions");
                const auto hasExtension=[&](const char* name) {
                    return std::any_of(ex.begin(),ex.end(),[&](const auto& x){return std::strcmp(x.extensionName,name)==0;});
                };
                if(!hasExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME) ||
                   (requestPresentId && !hasExtension(VK_KHR_PRESENT_ID_EXTENSION_NAME)) ||
                   (requestPresentRegions && !hasExtension(VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME))) continue;
                if(requestPresentId) {
                    VkPhysicalDevicePresentIdFeaturesKHR supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
                    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                    features.pNext=&supported;
                    vkGetPhysicalDeviceFeatures2(d,&features);
                    if(!supported.presentId) continue;
                }
                metadataSupportedDevice=true;
            }
#endif
            std::uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(d,&qn,nullptr); std::vector<VkQueueFamilyProperties> q(qn); vkGetPhysicalDeviceQueueFamilyProperties(d,&qn,q.data());
            for(std::uint32_t i=0;i<qn;++i) if(q[i].queueCount && (q[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)) {
#ifdef ZVRAM_GRAPHICS_SDL2
                if(present) { VkBool32 supported=VK_FALSE; check(vkGetPhysicalDeviceSurfaceSupportKHR(d,i,surface,&supported),"query presentation support"); if(!supported) continue; }
#endif
                physical=d; queueFamily=i; break;
            }
            if(physical) break;
        }
#ifdef ZVRAM_GRAPHICS_SDL2
        if(present && (requestPresentId||requestPresentRegions) && !metadataSupportedDevice) {
            unsupported=true;
            unsupportedReason="requested extension or feature is unavailable";
            return;
        }
#endif
        require(physical,"no graphics queue");
        VkPhysicalDeviceProperties selectedProperties{}; vkGetPhysicalDeviceProperties(physical,&selectedProperties);
        std::cout<<"graphics-device="<<selectedProperties.deviceName<<" type="<<static_cast<unsigned>(selectedProperties.deviceType)<<'\n';
        VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(physical,&mp);
        const VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,nullptr,0,queueFamily,1,&queuePriority};
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi;
#ifdef ZVRAM_GRAPHICS_SDL2
        std::vector<const char*> enabledDeviceExtensions;
        if(present) enabledDeviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        if(requestPresentId) enabledDeviceExtensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
        if(requestPresentRegions) enabledDeviceExtensions.push_back(VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME);
        if(present) {
            di.enabledExtensionCount=static_cast<std::uint32_t>(enabledDeviceExtensions.size());
            di.ppEnabledExtensionNames=enabledDeviceExtensions.data();
        }
        VkPhysicalDevicePresentIdFeaturesKHR enabledPresentId{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
        VkPhysicalDeviceFeatures2 enabledFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        if(requestPresentId) {
            VkPhysicalDeviceFeatures supportedBase{};
            vkGetPhysicalDeviceFeatures(physical,&supportedBase);
            enabledFeatures.features.sparseBinding=supportedBase.sparseBinding;
            enabledFeatures.features.sparseResidencyBuffer=supportedBase.sparseResidencyBuffer;
            enabledPresentId.presentId=VK_TRUE;
            enabledFeatures.pNext=&enabledPresentId;
            di.pNext=&enabledFeatures;
        }
#endif
        check(vkCreateDevice(physical,&di,nullptr,&device),"create device"); vkGetDeviceQueue(device,queueFamily,0,&queue);
        getStats=reinterpret_cast<GetStats>(vkGetDeviceProcAddr(device,"vkZVramGetSnapshotStatsNX"));
        if (!native) require(getStats,"zVram stats extension unavailable; use --native for standalone fixture");
        else getStats=nullptr;
#ifdef ZVRAM_GRAPHICS_SDL2
        if(present) createSwapchain(window);
#endif
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.queueFamilyIndex=queueFamily;
        check(vkCreateCommandPool(device,&cpi,nullptr,&pool),"create command pool");
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; check(vkCreateFence(device,&fci,nullptr,&fence),"create fence");
        data=makeBuffer(BufferBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,dataMem);
        upload=makeBuffer(BufferBytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,uploadMem);
        readback=makeBuffer(BufferBytes+Width*Height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,readbackMem);
        createImageAndPipeline();
    }
    std::uint32_t queueFamily{}; float queuePriority=1.f;

#ifdef ZVRAM_GRAPHICS_SDL2
    void createSwapchain(SDL_Window*) {
        VkSurfaceCapabilitiesKHR caps{}; check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps),"query surface capabilities");
        require((caps.supportedUsageFlags&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT))==(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT),"swapchain images lack render/readback usage");
        std::uint32_t count=0; check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,nullptr),"query surface formats");
        std::vector<VkSurfaceFormatKHR> formats(count); check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,formats.data()),"read surface formats");
        auto fmt=std::find_if(formats.begin(),formats.end(),[](const auto& f){return f.format==VK_FORMAT_R8G8B8A8_UNORM&&f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;});
        if(fmt==formats.end()) fmt=std::find_if(formats.begin(),formats.end(),[](const auto& f){return f.format==VK_FORMAT_B8G8R8A8_UNORM&&f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;});
        require(fmt!=formats.end(),"surface lacks RGBA8/BGRA8 UNORM/SRGB format"); colorFormat=fmt->format;
        extent=caps.currentExtent;
        if(extent.width==UINT32_MAX) { extent.width=std::clamp(Width,caps.minImageExtent.width,caps.maxImageExtent.width); extent.height=std::clamp(Height,caps.minImageExtent.height,caps.maxImageExtent.height); }
        require(extent.width==Width&&extent.height==Height,"surface extent is not 16x16");
        std::uint32_t imageCount=caps.minImageCount+1; if(caps.maxImageCount&&imageCount>caps.maxImageCount) imageCount=caps.maxImageCount;
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; ci.surface=surface; ci.minImageCount=imageCount; ci.imageFormat=colorFormat; ci.imageColorSpace=fmt->colorSpace; ci.imageExtent=extent; ci.imageArrayLayers=1;
        ci.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT; ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE; ci.preTransform=caps.currentTransform;
        ci.compositeAlpha=(caps.supportedCompositeAlpha&VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)?VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR:static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha&(~caps.supportedCompositeAlpha+1));
        ci.presentMode=VK_PRESENT_MODE_FIFO_KHR; ci.clipped=VK_TRUE;
        check(vkCreateSwapchainKHR(device,&ci,nullptr,&swapchain),"create swapchain");
        check(vkGetSwapchainImagesKHR(device,swapchain,&imageCount,nullptr),"query swapchain images"); swapImages.resize(imageCount);
        check(vkGetSwapchainImagesKHR(device,swapchain,&imageCount,swapImages.data()),"read swapchain images");
        swapViews.resize(imageCount); swapFramebuffers.resize(imageCount); acquireSemaphores.resize(imageCount); renderSemaphores.resize(imageCount);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=colorFormat; vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for(std::uint32_t i=0;i<imageCount;++i) { vi.image=swapImages[i]; check(vkCreateImageView(device,&vi,nullptr,&swapViews[i]),"create swapchain view"); check(vkCreateSemaphore(device,&sci,nullptr,&acquireSemaphores[i]),"create acquire semaphore"); check(vkCreateSemaphore(device,&sci,nullptr,&renderSemaphores[i]),"create render semaphore"); }
    }
#endif

    void createImageAndPipeline() {
#ifdef ZVRAM_GRAPHICS_SDL2
        if(presenting) {
            VkAttachmentDescription attachment{}; attachment.format=colorFormat; attachment.samples=VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}; VkSubpassDescription sub{}; sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount=1; sub.pColorAttachments=&ref;
            VkSubpassDependency dep{}; dep.srcSubpass=0; dep.dstSubpass=VK_SUBPASS_EXTERNAL; dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT; dep.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            // Acquire waits at COLOR_ATTACHMENT_OUTPUT; the initial layout
            // transition must participate in that execution dependency too.
            std::array<VkSubpassDependency,2> dependencies{};
            dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL; dependencies[0].dstSubpass=0;
            dependencies[0].srcStageMask=dependencies[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependencies[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dependencies[1]=dep;
            VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rp.attachmentCount=1; rp.pAttachments=&attachment; rp.subpassCount=1; rp.pSubpasses=&sub; rp.dependencyCount=2; rp.pDependencies=dependencies.data();
            check(vkCreateRenderPass(device,&rp,nullptr,&renderPass),"create present render pass");
            for(std::size_t i=0;i<swapViews.size();++i) { VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fb.renderPass=renderPass; fb.attachmentCount=1; fb.pAttachments=&swapViews[i]; fb.width=Width; fb.height=Height; fb.layers=1; check(vkCreateFramebuffer(device,&fb,nullptr,&swapFramebuffers[i]),"create swapchain framebuffer"); }
        } else
#endif
        {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType=VK_IMAGE_TYPE_2D; ii.format=VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent={Width,Height,1}; ii.mipLevels=1; ii.arrayLayers=1; ii.samples=VK_SAMPLE_COUNT_1_BIT; ii.tiling=VK_IMAGE_TILING_OPTIMAL;
        ii.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT; ii.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(device,&ii,nullptr,&image),"create target image"); VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(device,image,&mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize=mr.size; mai.memoryTypeIndex=memoryType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device,&mai,nullptr,&imageMem),"allocate target image"); check(vkBindImageMemory(device,image,imageMem,0),"bind target image");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; check(vkCreateImageView(device,&vi,nullptr,&view),"create image view");
        VkAttachmentDescription attachment{}; attachment.format=VK_FORMAT_R8G8B8A8_UNORM; attachment.samples=VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{}; sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount=1; sub.pColorAttachments=&ref;
        VkSubpassDependency dep{}; dep.srcSubpass=0; dep.dstSubpass=VK_SUBPASS_EXTERNAL; dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT; dep.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rp.attachmentCount=1; rp.pAttachments=&attachment; rp.subpassCount=1; rp.pSubpasses=&sub; rp.dependencyCount=1; rp.pDependencies=&dep;
        check(vkCreateRenderPass(device,&rp,nullptr,&renderPass),"create render pass");
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fb.renderPass=renderPass; fb.attachmentCount=1; fb.pAttachments=&view; fb.width=Width; fb.height=Height; fb.layers=1;
        check(vkCreateFramebuffer(device,&fb,nullptr,&framebuffer),"create framebuffer");
        }
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; sl.bindingCount=1; sl.pBindings=&binding;
        check(vkCreateDescriptorSetLayout(device,&sl,nullptr,&setLayout),"create descriptor layout");
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}; VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=1; dp.poolSizeCount=1; dp.pPoolSizes=&ps;
        check(vkCreateDescriptorPool(device,&dp,nullptr,&descriptorPool),"create descriptor pool");
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool=descriptorPool; da.descriptorSetCount=1; da.pSetLayouts=&setLayout;
        check(vkAllocateDescriptorSets(device,&da,&set),"allocate descriptor set"); VkDescriptorBufferInfo db{data,0,descriptorRange};
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; wr.dstSet=set; wr.dstBinding=0; wr.descriptorCount=1; wr.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr.pBufferInfo=&db;
        vkUpdateDescriptorSets(device,1,&wr,0,nullptr);
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount=1; pl.pSetLayouts=&setLayout;
        check(vkCreatePipelineLayout(device,&pl,nullptr,&pipelineLayout),"create pipeline layout");
        auto vert=readSpv(ZVRAM_GRAPHICS_VERT_PATH), frag=readSpv(ZVRAM_GRAPHICS_FRAG_PATH);
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=vert.size()*4; sm.pCode=vert.data(); VkShaderModule vs{};
        check(vkCreateShaderModule(device,&sm,nullptr,&vs),"create vertex shader"); sm.codeSize=frag.size()*4; sm.pCode=frag.data(); VkShaderModule fs{};
        try { check(vkCreateShaderModule(device,&sm,nullptr,&fs),"create fragment shader"); } catch(...) { vkDestroyShaderModule(device,vs,nullptr); throw; }
        VkPipelineShaderStageCreateInfo stages[2]{}; stages[0]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,vs,"main",nullptr}; stages[1]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,fs,"main",nullptr};
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0,0,float(Width),float(Height),0,1}; VkRect2D scissor{{0,0},{Width,Height}};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount=1; vp.pViewports=&viewport; vp.scissorCount=1; vp.pScissors=&scissor;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode=VK_POLYGON_MODE_FILL; raster.cullMode=VK_CULL_MODE_NONE; raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{}; blend.colorWriteMask=0xf;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount=1; cb.pAttachments=&blend;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gp.stageCount=2; gp.pStages=stages; gp.pVertexInputState=&vertex; gp.pInputAssemblyState=&assembly; gp.pViewportState=&vp; gp.pRasterizationState=&raster; gp.pMultisampleState=&ms; gp.pColorBlendState=&cb; gp.layout=pipelineLayout; gp.renderPass=renderPass; gp.subpass=0;
        const auto r=vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&pipeline); vkDestroyShaderModule(device,fs,nullptr); vkDestroyShaderModule(device,vs,nullptr); check(r,"create graphics pipeline");
    }

    void uploadInput(const std::vector<std::uint32_t>& input) {
        void* p{}; check(vkMapMemory(device,uploadMem,0,BufferBytes,0,&p),"map upload memory"); std::memcpy(p,input.data(),BufferBytes); vkUnmapMemory(device,uploadMem);
        auto cmd=begin(); VkBufferCopy c{0,0,BufferBytes}; vkCmdCopyBuffer(cmd,upload,data,1,&c);
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT; barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; barrier.buffer=data; barrier.size=BufferBytes;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&barrier,0,nullptr); submit(cmd);
    }
    void verifyFullBuffer(const std::vector<std::uint32_t>& expected) {
        auto cmd=begin();
        VkBufferMemoryBarrier dataBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        dataBarrier.srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
        dataBarrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        dataBarrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; dataBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        dataBarrier.buffer=data; dataBarrier.size=BufferBytes;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&dataBarrier,0,nullptr);
        VkBufferCopy copy{0,0,BufferBytes}; vkCmdCopyBuffer(cmd,data,readback,1,&copy);
        VkBufferMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hostBarrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; hostBarrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        hostBarrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; hostBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        hostBarrier.buffer=readback; hostBarrier.size=BufferBytes;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
                             0,0,nullptr,1,&hostBarrier,0,nullptr);
        submit(cmd);
        void* p{}; check(vkMapMemory(device,readbackMem,0,BufferBytes,0,&p),"map full-buffer verification");
        const auto* words=static_cast<const std::uint32_t*>(p);
        const bool matches=std::equal(expected.begin(),expected.end(),words);
        vkUnmapMemory(device,readbackMem);
        require(matches,"32 MiB full-buffer integrity mismatch");
    }
    void drawAndVerify(const std::vector<std::uint32_t>& expected, unsigned frameIndex,
                       bool verifyFullBuffer=true) {
        std::uint32_t imageIndex=0; VkSemaphore acquired=VK_NULL_HANDLE, rendered=VK_NULL_HANDLE;
#ifndef ZVRAM_GRAPHICS_SDL2
        (void)imageIndex; (void)frameIndex;
#endif
#ifdef ZVRAM_GRAPHICS_SDL2
        if(presenting) SDL_PumpEvents();
        if(presenting) { acquired=acquireSemaphores[frameIndex%acquireSemaphores.size()]; const auto a=vkAcquireNextImageKHR(device,swapchain,3'000'000'000ull,acquired,VK_NULL_HANDLE,&imageIndex); if(a==VK_TIMEOUT) throw std::runtime_error("swapchain image acquire timed out"); if(a!=VK_SUCCESS&&a!=VK_SUBOPTIMAL_KHR) check(a,"acquire swapchain image"); rendered=renderSemaphores[imageIndex]; }
#endif
        auto cmd=begin(); VkClearValue clear{}; VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rp.renderPass=renderPass;
#ifdef ZVRAM_GRAPHICS_SDL2
        rp.framebuffer=presenting?swapFramebuffers[imageIndex]:framebuffer;
#else
        rp.framebuffer=framebuffer;
#endif
        rp.renderArea={{0,0},{Width,Height}}; rp.clearValueCount=1; rp.pClearValues=&clear;
        vkCmdBeginRenderPass(cmd,&rp,VK_SUBPASS_CONTENTS_INLINE); vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline); vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&set,0,nullptr); vkCmdDraw(cmd,3,1,0,0); vkCmdEndRenderPass(cmd);
        if(verifyFullBuffer) {
            VkBufferMemoryBarrier dataBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; dataBarrier.srcAccessMask=VK_ACCESS_SHADER_READ_BIT; dataBarrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT; dataBarrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; dataBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; dataBarrier.buffer=data; dataBarrier.size=BufferBytes;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&dataBarrier,0,nullptr);
        }
        VkBufferImageCopy imageCopy{}; imageCopy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; imageCopy.imageExtent={Width,Height,1};
#ifdef ZVRAM_GRAPHICS_SDL2
        const VkImage target=presenting?swapImages[imageIndex]:image;
#else
        const VkImage target=image;
#endif
        vkCmdCopyImageToBuffer(cmd,target,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&imageCopy);
        if(verifyFullBuffer) { VkBufferCopy dataCopy{0,Width*Height*4,BufferBytes}; vkCmdCopyBuffer(cmd,data,readback,1,&dataCopy); }
#ifdef ZVRAM_GRAPHICS_SDL2
        if(presenting) { VkImageMemoryBarrier presentBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; presentBarrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; presentBarrier.dstAccessMask=0; presentBarrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; presentBarrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; presentBarrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; presentBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; presentBarrier.image=target; presentBarrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&presentBarrier); }
#endif
        VkBufferMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; hostBarrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; hostBarrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT; hostBarrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; hostBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; hostBarrier.buffer=readback; hostBarrier.size=verifyFullBuffer?BufferBytes+Width*Height*4:Width*Height*4;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&hostBarrier,0,nullptr);
        submit(cmd,acquired,rendered,true);
#ifdef ZVRAM_GRAPHICS_SDL2
        if(presenting) {
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount=1; pi.pWaitSemaphores=&rendered;
            pi.swapchainCount=1; pi.pSwapchains=&swapchain; pi.pImageIndices=&imageIndex;
            VkPresentIdKHR idInfo{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
            const std::uint64_t presentId=nextPresentId++;
            if(presentIdMetadata) { idInfo.swapchainCount=1; idInfo.pPresentIds=&presentId; }
            VkRectLayerKHR rectangle{}; rectangle.offset={0,0}; rectangle.extent=extent; rectangle.layer=0;
            VkPresentRegionKHR region{}; region.rectangleCount=1; region.pRectangles=&rectangle;
            VkPresentRegionsKHR regionsInfo{VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR};
            if(presentRegionsMetadata) { regionsInfo.swapchainCount=1; regionsInfo.pRegions=&region; }
            if(presentIdMetadata && presentRegionsMetadata) idInfo.pNext=&regionsInfo;
            pi.pNext=presentIdMetadata ? static_cast<const void*>(&idInfo) :
                     (presentRegionsMetadata ? static_cast<const void*>(&regionsInfo) : nullptr);
            const auto pr=vkQueuePresentKHR(queue,&pi);
            if(pr!=VK_SUCCESS&&pr!=VK_SUBOPTIMAL_KHR) abandon=true;
            if(pr!=VK_SUCCESS&&pr!=VK_SUBOPTIMAL_KHR) check(pr,"present swapchain image");
        }
#endif
        void* p{}; check(vkMapMemory(device,readbackMem,0,verifyFullBuffer?BufferBytes+Width*Height*4:Width*Height*4,0,&p),"map readback");
        const auto* pixels=static_cast<const std::uint8_t*>(p);
#ifdef ZVRAM_GRAPHICS_SDL2
        const std::array<std::uint8_t,4> rgba=colorFormat==VK_FORMAT_B8G8R8A8_UNORM?std::array<std::uint8_t,4>{223,128,32,255}:std::array<std::uint8_t,4>{32,128,223,255};
#else
        const std::array<std::uint8_t,4> rgba{32,128,223,255};
#endif
        for(std::size_t i=0;i<Width*Height;++i) require(std::memcmp(pixels+i*4,rgba.data(),4)==0,"unexpected rendered pixel");
        if(verifyFullBuffer) {
            const auto* words=reinterpret_cast<const std::uint32_t*>(pixels+Width*Height*4);
            require(std::equal(expected.begin(),expected.end(),words),"32 MiB native buffer readback mismatch");
        }
        vkUnmapMemory(device,readbackMem);
    }
};
}

struct UnsupportedMetadata {};

int main(int argc,char** argv) {
    bool native=false, present=false, nativeAllocation=false, expectLazy=false, warmResidency=false;
    std::string presentMetadata="none";
    unsigned frames=3, frameDelayMs=0, descriptorWindowMiB=0;
    bool framesSpecified=false, expectConservativeGraphics=false;
#ifdef ZVRAM_GRAPHICS_SDL2
    SDL_Window* window=nullptr; bool sdlReady=false;
#endif
    try {
        for(int i=1;i<argc;++i) {
            if(std::strcmp(argv[i],"--native")==0) native=true;
            else if(std::strcmp(argv[i],"--native-allocation")==0) nativeAllocation=true;
            else if(std::strcmp(argv[i],"--expect-lazy-backing")==0) expectLazy=true;
            else if(std::strcmp(argv[i],"--frames")==0 && i+1<argc) { frames=static_cast<unsigned>(std::stoul(argv[++i])); framesSpecified=true; require(frames>=2 && frames<=120,"--frames must be 2..120"); }
            else if(std::strcmp(argv[i],"--descriptor-window-mib")==0 && i+1<argc) descriptorWindowMiB=static_cast<unsigned>(std::stoul(argv[++i]));
            else if(std::strcmp(argv[i],"--expect-conservative-graphics")==0) expectConservativeGraphics=true;
            else if(std::strcmp(argv[i],"--warm-residency")==0) warmResidency=true;
            else if(std::strcmp(argv[i],"--present-metadata")==0 && i+1<argc) presentMetadata=argv[++i];
            else if(std::strcmp(argv[i],"--frame-delay-ms")==0 && i+1<argc) { frameDelayMs=static_cast<unsigned>(std::stoul(argv[++i])); require(frameDelayMs<=1000,"frame delay maximum is 1000 ms"); }
#ifdef ZVRAM_GRAPHICS_SDL2
            else if(std::strcmp(argv[i],"--present")==0) present=true;
#endif
            else throw std::runtime_error("usage: vulkan-graphics-check [--native] [--native-allocation] [--expect-lazy-backing] [--present] [--present-metadata id|regions|both] [--warm-residency] [--frame-delay-ms 0..1000] [--frames 2..120]");
        }
        require(presentMetadata=="none" || presentMetadata=="id" ||
                presentMetadata=="regions" || presentMetadata=="both",
                "--present-metadata must be id, regions or both");
        const bool requestPresentId=presentMetadata=="id" || presentMetadata=="both";
        const bool requestPresentRegions=presentMetadata=="regions" || presentMetadata=="both";
        const bool narrowWorkingSet=descriptorWindowMiB!=0;
        if(narrowWorkingSet) {
            require(descriptorWindowMiB==4,"--descriptor-window-mib supports only 4");
            require(!native && !present && presentMetadata=="none" && !warmResidency && !expectLazy,
                    "descriptor-window mode requires wrapped offscreen cold recovery");
            require(!framesSpecified || frames==6,"descriptor-window mode requires exactly 6 frames");
            frames=6;
        }
        require(!expectConservativeGraphics || narrowWorkingSet,
                "--expect-conservative-graphics requires --descriptor-window-mib 4");
#ifndef ZVRAM_GRAPHICS_SDL2
        if(presentMetadata!="none") {
            std::cerr<<"UNSUPPORTED: PRESENT_METADATA: requires SDL2 support\n";
            return 77;
        }
#else
        require(present || presentMetadata=="none","--present-metadata requires --present");
#endif
        require(!expectLazy || !native,"lazy backing check requires zVram");
#ifdef ZVRAM_GRAPHICS_SDL2
        if(present) { require(SDL_Init(SDL_INIT_VIDEO)==0,"initialize SDL2 video"); sdlReady=true; window=SDL_CreateWindow("zVram Vulkan graphics check",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,Width,Height,SDL_WINDOW_VULKAN|SDL_WINDOW_SHOWN); require(window,"create SDL Vulkan window"); SDL_PumpEvents(); }
#else
        (void)present;
#endif
        bool validationOn=false;
        {
            Fixture f;
            f.nativeAllocation=nativeAllocation;
            if(narrowWorkingSet) f.descriptorRange=4ull*1024*1024;
            f.init(native,requestPresentId,requestPresentRegions
#ifdef ZVRAM_GRAPHICS_SDL2
                   ,present,window
#endif
                   );
            if(f.unsupported) {
                std::cerr<<"UNSUPPORTED: PRESENT_METADATA: "<<f.unsupportedReason<<'\n';
                throw UnsupportedMetadata{};
            }
            auto input=makeInput(); const auto initial=native?Stats{}:f.stats();
            if(expectLazy) {
                require(initial.residentBytes==0 && initial.coldLogicalBytes==BufferBytes && initial.coldStoredBytes==0,
                        "lazy bootstrap allocated backing or captured undefined contents");
                std::cout<<"PASS: lazy bootstrap resident=0 cold-logical="<<initial.coldLogicalBytes<<" cold-stored=0\n";
            }
            f.uploadInput(input);
            if(narrowWorkingSet) f.verifyFullBuffer(input);
            if(!native && !warmResidency) f.waitCold(initial.freezes);
            if(narrowWorkingSet) {
                const auto cold=f.stats();
                require(cold.coldLogicalBytes==BufferBytes && cold.residentBytes==0,
                        "narrow fixture did not begin with all 32 MiB cold");
            }
            std::vector<double> frameTimes;
            f.submitCallMicros.reserve(frames);
            f.fenceWaitCallMicros.reserve(frames);
            for(unsigned i=0;i<frames;++i) {
                if(frameDelayMs) std::this_thread::sleep_for(std::chrono::milliseconds(frameDelayMs));
                const auto before=native?Stats{}:f.stats();
                if(narrowWorkingSet && i>0) {
                    const auto expectedCold=expectConservativeGraphics?0:BufferBytes-4ull*1024*1024;
                    const auto expectedResident=expectConservativeGraphics?BufferBytes:4ull*1024*1024;
                    require(before.coldLogicalBytes==expectedCold && before.residentBytes==expectedResident,
                            "graphics residency changed before measured draw");
                }
                else if(!native && !warmResidency) require(before.coldLogicalBytes>=BufferBytes && before.residentBytes==0,"draw did not begin fully cold");
                if(!native && warmResidency) require(before.residentBytes>=BufferBytes && before.coldLogicalBytes==0 && before.freezes==initial.freezes,"warm graphics data was needlessly evicted");
                const auto start=std::chrono::steady_clock::now();
                f.drawAndVerify(input,i,!narrowWorkingSet);
                frameTimes.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
                if(narrowWorkingSet) {
                    const auto after=f.stats();
                    if(expectConservativeGraphics) {
                        if(i==0) require(after.coldLogicalBytes==0 && after.residentBytes==BufferBytes &&
                                         after.restores==initial.restores+BufferBytes/(4ull*1024*1024),
                                         "first graphics draw did not conservatively restore all eight chunks");
                        else require(after.coldLogicalBytes==0 && after.residentBytes==BufferBytes &&
                                     after.restores==before.restores,
                                     "conservatively restored graphics data was not reused");
                        std::cout<<"conservative-frame="<<i<<" resident="<<after.residentBytes
                                 <<" cold="<<after.coldLogicalBytes<<" restores="<<after.restores<<'\n';
                    } else {
                        require(after.coldLogicalBytes==BufferBytes-4ull*1024*1024 && after.residentBytes==4ull*1024*1024,
                                "measured draw did not retain exactly its 4 MiB descriptor chunk");
                        if(i==0) require(after.restores==before.restores+1,
                                         "first narrow draw did not restore exactly one descriptor chunk");
                        else require(after.restores==before.restores,
                                     "warm narrow draw performed an unexpected restore");
                        std::cout<<"narrow-frame="<<i<<" resident="<<after.residentBytes
                                 <<" cold="<<after.coldLogicalBytes<<" restores="<<after.restores<<'\n';
                    }
                } else if(!native && !warmResidency) { const auto after=f.stats(); require(after.restores>before.restores,"graphics use did not restore cold buffer"); std::cout<<"frame="<<i<<" cold-bytes="<<after.coldStoredBytes<<'/'<<after.coldLogicalBytes<<" restores="<<after.restores<<'\n'; f.waitCold(before.freezes); }
            }
            if(narrowWorkingSet) {
                const auto beforeIntegrityReadback=f.stats();
                if(expectConservativeGraphics)
                    require(beforeIntegrityReadback.coldLogicalBytes==0 && beforeIntegrityReadback.residentBytes==BufferBytes,
                            "conservative residency changed before final integrity readback");
                else require(beforeIntegrityReadback.coldLogicalBytes==BufferBytes-4ull*1024*1024 &&
                             beforeIntegrityReadback.residentBytes==4ull*1024*1024,
                             "narrow working-set accounting changed before final integrity readback");
                f.verifyFullBuffer(input);
            }
            std::sort(frameTimes.begin(),frameTimes.end());
            std::cout<<"draw-readback-ms p50="<<frameTimes[frameTimes.size()/2]<<" p95="<<frameTimes[(frameTimes.size()*95+99)/100-1]<<" max="<<frameTimes.back()<<'\n';
            const auto printCallTimes=[](const char* name,std::vector<std::uint64_t>& samples) {
                require(!samples.empty(),"graphics frame timing collected no samples");
                std::sort(samples.begin(),samples.end());
                const auto percentile=[&](std::size_t percent) {
                    const auto rank=(samples.size()*percent+99)/100;
                    return samples[std::max<std::size_t>(1,rank)-1];
                };
                std::cout<<name<<" count="<<samples.size()<<" p50-us="<<percentile(50)
                         <<" p95-us="<<percentile(95)<<" max-us="<<samples.back()<<'\n';
            };
            if(narrowWorkingSet && f.submitCallMicros.size()>1 && f.fenceWaitCallMicros.size()>1) {
                std::vector<std::uint64_t> firstSubmit{f.submitCallMicros.front()};
                std::vector<std::uint64_t> warmSubmit(f.submitCallMicros.begin()+1,f.submitCallMicros.end());
                std::vector<std::uint64_t> firstWait{f.fenceWaitCallMicros.front()};
                std::vector<std::uint64_t> warmWait(f.fenceWaitCallMicros.begin()+1,f.fenceWaitCallMicros.end());
                printCallTimes("first-vkQueueSubmit-call",firstSubmit);
                printCallTimes("warm-vkQueueSubmit-call",warmSubmit);
                printCallTimes("first-vkWaitForFences-call",firstWait);
                printCallTimes("warm-vkWaitForFences-call",warmWait);
            }
            printCallTimes("vkQueueSubmit-call",f.submitCallMicros);
            printCallTimes("vkWaitForFences-call",f.fenceWaitCallMicros);
            validationOn=f.validationOn;
        }
        require(validationErrors.load()==0,"Vulkan validation reported errors");
#ifdef ZVRAM_GRAPHICS_SDL2
        if(window) SDL_DestroyWindow(window);
        if(sdlReady) SDL_Quit();
#endif
        std::cout<<"PASS: "<<frames<<(present?" presented":" offscreen")<<" draw/readback frames"<<(native?" (native mode)":(warmResidency?" with warm residency":(expectConservativeGraphics?" with conservative graphics fallback":" with cold restore")))
                 <<(presentMetadata!="none"?" metadata="+presentMetadata:"")
                 <<" validation="<<(validationOn?"on":"unavailable")<<"\n";
        return 0;
    } catch(const UnsupportedMetadata&) {
#ifdef ZVRAM_GRAPHICS_SDL2
        if(window) SDL_DestroyWindow(window);
        if(sdlReady) SDL_Quit();
#endif
        return 77;
    } catch(const std::exception& e) {
#ifdef ZVRAM_GRAPHICS_SDL2
        if(window) SDL_DestroyWindow(window);
        if(sdlReady) SDL_Quit();
#endif
        std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1;
    }
}
