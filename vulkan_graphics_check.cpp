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
    bool presenting{}, nativeAllocation{};
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
                VkSemaphore signalSemaphore=VK_NULL_HANDLE) {
        check(vkEndCommandBuffer(cmd),"end command buffer"); check(vkResetFences(device,1,&fence),"reset fence");
        const VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        if(waitSemaphore) { si.waitSemaphoreCount=1; si.pWaitSemaphores=&waitSemaphore; si.pWaitDstStageMask=&waitStage; }
        si.commandBufferCount=1; si.pCommandBuffers=&cmd;
        if(signalSemaphore) { si.signalSemaphoreCount=1; si.pSignalSemaphores=&signalSemaphore; }
        const auto submitted=vkQueueSubmit(queue,1,&si,fence);
        if (submitted==VK_ERROR_DEVICE_LOST) abandon=true;
        check(submitted,"submit graphics fixture");
        const auto r=vkWaitForFences(device,1,&fence,VK_TRUE,3'000'000'000ull);
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
    void init(bool native
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
        for(auto d:devices) {
#ifdef ZVRAM_GRAPHICS_SDL2
            if(present) { std::uint32_t ec=0; check(vkEnumerateDeviceExtensionProperties(d,nullptr,&ec,nullptr),"query device extensions"); std::vector<VkExtensionProperties> ex(ec); check(vkEnumerateDeviceExtensionProperties(d,nullptr,&ec,ex.data()),"read device extensions"); if(std::none_of(ex.begin(),ex.end(),[](const auto& x){return std::strcmp(x.extensionName,VK_KHR_SWAPCHAIN_EXTENSION_NAME)==0;})) continue; }
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
        require(physical,"no graphics queue");
        VkPhysicalDeviceProperties selectedProperties{}; vkGetPhysicalDeviceProperties(physical,&selectedProperties);
        std::cout<<"graphics-device="<<selectedProperties.deviceName<<" type="<<static_cast<unsigned>(selectedProperties.deviceType)<<'\n';
        VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(physical,&mp);
        const VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,nullptr,0,queueFamily,1,&queuePriority};
#ifdef ZVRAM_GRAPHICS_SDL2
        const char* swapExt=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
#endif
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi;
#ifdef ZVRAM_GRAPHICS_SDL2
        if(present) { di.enabledExtensionCount=1; di.ppEnabledExtensionNames=&swapExt; }
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
            VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rp.attachmentCount=1; rp.pAttachments=&attachment; rp.subpassCount=1; rp.pSubpasses=&sub; rp.dependencyCount=1; rp.pDependencies=&dep;
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
        check(vkAllocateDescriptorSets(device,&da,&set),"allocate descriptor set"); VkDescriptorBufferInfo db{data,0,BufferBytes};
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
    void drawAndVerify(const std::vector<std::uint32_t>& expected, unsigned frameIndex) {
        std::uint32_t imageIndex=0; VkSemaphore acquired=VK_NULL_HANDLE, rendered=VK_NULL_HANDLE;
#ifndef ZVRAM_GRAPHICS_SDL2
        (void)imageIndex; (void)frameIndex;
#endif
#ifdef ZVRAM_GRAPHICS_SDL2
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
        VkBufferMemoryBarrier dataBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; dataBarrier.srcAccessMask=VK_ACCESS_SHADER_READ_BIT; dataBarrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT; dataBarrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; dataBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; dataBarrier.buffer=data; dataBarrier.size=BufferBytes;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&dataBarrier,0,nullptr);
        VkBufferImageCopy imageCopy{}; imageCopy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; imageCopy.imageExtent={Width,Height,1};
#ifdef ZVRAM_GRAPHICS_SDL2
        const VkImage target=presenting?swapImages[imageIndex]:image;
#else
        const VkImage target=image;
#endif
        vkCmdCopyImageToBuffer(cmd,target,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&imageCopy);
        VkBufferCopy dataCopy{0,Width*Height*4,BufferBytes}; vkCmdCopyBuffer(cmd,data,readback,1,&dataCopy);
#ifdef ZVRAM_GRAPHICS_SDL2
        if(presenting) { VkImageMemoryBarrier presentBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; presentBarrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; presentBarrier.dstAccessMask=0; presentBarrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; presentBarrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; presentBarrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; presentBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; presentBarrier.image=target; presentBarrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&presentBarrier); }
#endif
        VkBufferMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; hostBarrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; hostBarrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT; hostBarrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; hostBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; hostBarrier.buffer=readback; hostBarrier.size=BufferBytes+Width*Height*4;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&hostBarrier,0,nullptr);
        submit(cmd,acquired,rendered);
#ifdef ZVRAM_GRAPHICS_SDL2
        if(presenting) { VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.waitSemaphoreCount=1; pi.pWaitSemaphores=&rendered; pi.swapchainCount=1; pi.pSwapchains=&swapchain; pi.pImageIndices=&imageIndex; const auto pr=vkQueuePresentKHR(queue,&pi); if(pr!=VK_SUCCESS&&pr!=VK_SUBOPTIMAL_KHR) abandon=true; if(pr!=VK_SUCCESS&&pr!=VK_SUBOPTIMAL_KHR) check(pr,"present swapchain image"); }
#endif
        void* p{}; check(vkMapMemory(device,readbackMem,0,BufferBytes+Width*Height*4,0,&p),"map readback");
        const auto* pixels=static_cast<const std::uint8_t*>(p);
#ifdef ZVRAM_GRAPHICS_SDL2
        const std::array<std::uint8_t,4> rgba=colorFormat==VK_FORMAT_B8G8R8A8_UNORM?std::array<std::uint8_t,4>{223,128,32,255}:std::array<std::uint8_t,4>{32,128,223,255};
#else
        const std::array<std::uint8_t,4> rgba{32,128,223,255};
#endif
        for(std::size_t i=0;i<Width*Height;++i) require(std::memcmp(pixels+i*4,rgba.data(),4)==0,"unexpected rendered pixel");
        const auto* words=reinterpret_cast<const std::uint32_t*>(pixels+Width*Height*4);
        require(std::equal(expected.begin(),expected.end(),words),"32 MiB native buffer readback mismatch"); vkUnmapMemory(device,readbackMem);
    }
};
}

int main(int argc,char** argv) {
    bool native=false, present=false, nativeAllocation=false; unsigned frames=3;
#ifdef ZVRAM_GRAPHICS_SDL2
    SDL_Window* window=nullptr; bool sdlReady=false;
#endif
    try {
        for(int i=1;i<argc;++i) {
            if(std::strcmp(argv[i],"--native")==0) native=true;
            else if(std::strcmp(argv[i],"--native-allocation")==0) nativeAllocation=true;
            else if(std::strcmp(argv[i],"--frames")==0 && i+1<argc) { frames=static_cast<unsigned>(std::stoul(argv[++i])); require(frames>=2 && frames<=3,"--frames must be 2 or 3"); }
#ifdef ZVRAM_GRAPHICS_SDL2
            else if(std::strcmp(argv[i],"--present")==0) present=true;
#endif
            else throw std::runtime_error("usage: vulkan-graphics-check [--native] [--native-allocation] [--present] [--frames 2|3]");
        }
#ifdef ZVRAM_GRAPHICS_SDL2
        if(present) { require(SDL_Init(SDL_INIT_VIDEO)==0,"initialize SDL2 video"); sdlReady=true; window=SDL_CreateWindow("zVram Vulkan graphics check",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,Width,Height,SDL_WINDOW_VULKAN|SDL_WINDOW_SHOWN); require(window,"create SDL Vulkan window"); }
#else
        (void)present;
#endif
        bool validationOn=false;
        {
            Fixture f;
            f.nativeAllocation=nativeAllocation;
            f.init(native
#ifdef ZVRAM_GRAPHICS_SDL2
                   ,present,window
#endif
                   );
            auto input=makeInput(); const auto initial=native?Stats{}:f.stats(); f.uploadInput(input);
            if(!native) f.waitCold(initial.freezes);
            for(unsigned i=0;i<frames;++i) {
                const auto before=native?Stats{}:f.stats();
                if(!native) require(before.coldLogicalBytes>=BufferBytes && before.residentBytes==0,"draw did not begin fully cold");
                f.drawAndVerify(input,i);
                if(!native) { const auto after=f.stats(); require(after.restores>before.restores,"graphics use did not restore cold buffer"); std::cout<<"frame="<<i<<" cold-bytes="<<after.coldStoredBytes<<'/'<<after.coldLogicalBytes<<" restores="<<after.restores<<'\n'; f.waitCold(before.freezes); }
            }
            validationOn=f.validationOn;
        }
        require(validationErrors.load()==0,"Vulkan validation reported errors");
#ifdef ZVRAM_GRAPHICS_SDL2
        if(window) SDL_DestroyWindow(window);
        if(sdlReady) SDL_Quit();
#endif
        std::cout<<"PASS: "<<frames<<(present?" presented":" offscreen")<<" draw/readback frames"<<(native?" (native mode)":" with cold restore")<<" validation="<<(validationOn?"on":"unavailable")<<"\n";
        return 0;
    } catch(const std::exception& e) {
#ifdef ZVRAM_GRAPHICS_SDL2
        if(window) SDL_DestroyWindow(window);
        if(sdlReady) SDL_Quit();
#endif
        std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1;
    }
}
