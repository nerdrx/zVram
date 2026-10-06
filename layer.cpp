#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
constexpr char kLayer[] = "VK_LAYER_NX_zvram";
template<class H> void* key(H h) { return h ? *reinterpret_cast<void**>(h) : nullptr; }
bool verbose() { const char* p=std::getenv("ZVRAM_VERBOSE"); return p && std::strcmp(p,"1")==0; }
void logf(const char* fmt,...) { std::fputs("[zvram] ",stderr); va_list ap; va_start(ap,fmt); std::vfprintf(stderr,fmt,ap); va_end(ap); std::fputc('\n',stderr); }

struct Instance {
    VkInstance handle{};
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_GetPhysicalDeviceProcAddr physProc{};
    PFN_vkDestroyInstance destroy{};
    PFN_vkEnumerateDeviceExtensionProperties enumerateExtensions{};
    PFN_vkGetPhysicalDeviceProperties properties{};
    PFN_vkGetPhysicalDeviceMemoryProperties memoryProperties{};
};
struct Allocation { VkDeviceSize size; bool local; };
struct Device {
    VkDevice handle{};
    PFN_vkGetDeviceProcAddr gdpa{};
    PFN_vkDestroyDevice destroy{};
    PFN_vkAllocateMemory allocate{};
    PFN_vkFreeMemory free{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::string gpu;
    bool autoPolicy{};
    std::mutex mutex;
    std::unordered_map<VkDeviceMemory,Allocation> allocations;
    uint64_t liveLocal{}, peakLocal{}, liveOther{}, peakOther{}, failures{};
};
std::mutex mapsMutex;
PFN_vkGetInstanceProcAddr globalGipa{};
std::unordered_map<void*,std::shared_ptr<Instance>> instances;
std::unordered_map<void*,std::shared_ptr<Device>> devices;

std::shared_ptr<Instance> findInstance(void* k) {
    std::lock_guard<std::mutex> lock(mapsMutex); auto i=instances.find(k); return i==instances.end()?nullptr:i->second;
}
std::shared_ptr<Device> findDevice(VkDevice d) {
    std::lock_guard<std::mutex> lock(mapsMutex); auto i=devices.find(key(d)); return i==devices.end()?nullptr:i->second;
}
VkLayerInstanceCreateInfo* instanceLink(const VkInstanceCreateInfo* ci) {
    for(auto* p=static_cast<const VkBaseInStructure*>(ci->pNext);p;p=p->pNext)
        if(p->sType==VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
            auto* l=reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<VkBaseInStructure*>(p));
            if(l->function==VK_LAYER_LINK_INFO) return l;
        }
    return nullptr;
}
VkLayerDeviceCreateInfo* deviceLink(const VkDeviceCreateInfo* ci) {
    for(auto* p=static_cast<const VkBaseInStructure*>(ci->pNext);p;p=p->pNext)
        if(p->sType==VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            auto* l=reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<VkBaseInStructure*>(p));
            if(l->function==VK_LAYER_LINK_INFO) return l;
        }
    return nullptr;
}
bool hasPolicy(const void* chain) {
    for(auto* p=static_cast<const VkBaseInStructure*>(chain);p;p=p->pNext)
        if(p->sType==VK_STRUCTURE_TYPE_DEVICE_MEMORY_OVERALLOCATION_CREATE_INFO_AMD) return true;
    return false;
}
bool appEnabled(const VkDeviceCreateInfo* ci,const char* name) {
    for(uint32_t i=0;i<ci->enabledExtensionCount;i++) if(std::strcmp(ci->ppEnabledExtensionNames[i],name)==0) return true;
    return false;
}

VKAPI_ATTR VkResult VKAPI_CALL layerCreateInstance(const VkInstanceCreateInfo*,const VkAllocationCallbacks*,VkInstance*);
VKAPI_ATTR void VKAPI_CALL layerDestroyInstance(VkInstance,const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL layerCreateDevice(VkPhysicalDevice,const VkDeviceCreateInfo*,const VkAllocationCallbacks*,VkDevice*);
VKAPI_ATTR void VKAPI_CALL layerDestroyDevice(VkDevice,const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL layerAllocateMemory(VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory*);
VKAPI_ATTR void VKAPI_CALL layerFreeMemory(VkDevice,VkDeviceMemory,const VkAllocationCallbacks*);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGetInstanceProcAddr(VkInstance,const char*);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGetDeviceProcAddr(VkDevice,const char*);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGetPhysicalDeviceProcAddr(VkInstance,const char*);

VKAPI_ATTR VkResult VKAPI_CALL layerCreateInstance(const VkInstanceCreateInfo* ci,const VkAllocationCallbacks* allocator,VkInstance* out) {
    if(!ci || !out) return VK_ERROR_INITIALIZATION_FAILED;
    auto* link=instanceLink(ci); if(!link || !link->u.pLayerInfo) { logf("missing loader instance link information"); return VK_ERROR_INITIALIZATION_FAILED; }
    auto* info=link->u.pLayerInfo; auto next=info->pfnNextGetInstanceProcAddr; auto nextPhys=info->pfnNextGetPhysicalDeviceProcAddr;
    link->u.pLayerInfo=info->pNext;
    auto create=reinterpret_cast<PFN_vkCreateInstance>(next(VK_NULL_HANDLE,"vkCreateInstance"));
    if(!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=create(ci,allocator,out); if(r!=VK_SUCCESS) return r;
    try {
        auto s=std::make_shared<Instance>(); s->handle=*out; s->gipa=next; s->physProc=nextPhys;
        s->destroy=reinterpret_cast<PFN_vkDestroyInstance>(next(*out,"vkDestroyInstance"));
        s->enumerateExtensions=reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(next(*out,"vkEnumerateDeviceExtensionProperties"));
        s->properties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(next(*out,"vkGetPhysicalDeviceProperties"));
        s->memoryProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(next(*out,"vkGetPhysicalDeviceMemoryProperties"));
        std::lock_guard<std::mutex> lock(mapsMutex); instances[key(*out)]=std::move(s); globalGipa=next;
    } catch(const std::bad_alloc&) {
        auto destroy=reinterpret_cast<PFN_vkDestroyInstance>(next(*out,"vkDestroyInstance")); if(destroy) destroy(*out,allocator);
        *out=VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL layerDestroyInstance(VkInstance instance,const VkAllocationCallbacks* allocator) {
    auto s=findInstance(key(instance)); if(!s) return;
    { std::lock_guard<std::mutex> lock(mapsMutex); instances.erase(key(instance)); }
    if(s->destroy) s->destroy(instance,allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL layerCreateDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo* ci,const VkAllocationCallbacks* allocator,VkDevice* out) {
    if(!ci || !out) return VK_ERROR_INITIALIZATION_FAILED;
    auto in=findInstance(key(physical)); if(!in) return VK_ERROR_INITIALIZATION_FAILED;
    auto* link=deviceLink(ci); if(!link || !link->u.pLayerInfo) { logf("missing loader device link information"); return VK_ERROR_INITIALIZATION_FAILED; }
    auto* info=link->u.pLayerInfo; auto nextGdpa=info->pfnNextGetDeviceProcAddr; auto nextGipa=info->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo=info->pNext;
    bool policy=hasPolicy(ci->pNext), supported=false;
    if(in->enumerateExtensions) {
        uint32_t count=0; VkResult er=in->enumerateExtensions(physical,nullptr,&count,nullptr);
        if(er==VK_SUCCESS || er==VK_INCOMPLETE) {
            try { std::vector<VkExtensionProperties> exts(count); er=in->enumerateExtensions(physical,nullptr,&count,exts.data());
                if(er==VK_SUCCESS || er==VK_INCOMPLETE) for(const auto& e:exts) if(std::strcmp(e.extensionName,VK_AMD_MEMORY_OVERALLOCATION_BEHAVIOR_EXTENSION_NAME)==0) { supported=true; break; }
            } catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
        }
    }
    bool inject=supported && !policy;
    if(!supported) logf("AMD overallocation extension unavailable; passing device creation through unchanged");
    VkDeviceCreateInfo copy=*ci; VkDeviceMemoryOverallocationCreateInfoAMD behavior{VK_STRUCTURE_TYPE_DEVICE_MEMORY_OVERALLOCATION_CREATE_INFO_AMD};
    std::vector<const char*> extensions;
    if(inject) {
        try {
            if(!appEnabled(ci,VK_AMD_MEMORY_OVERALLOCATION_BEHAVIOR_EXTENSION_NAME)) {
                if(ci->enabledExtensionCount) extensions.assign(ci->ppEnabledExtensionNames,ci->ppEnabledExtensionNames+ci->enabledExtensionCount);
                extensions.push_back(VK_AMD_MEMORY_OVERALLOCATION_BEHAVIOR_EXTENSION_NAME);
                copy.enabledExtensionCount=static_cast<uint32_t>(extensions.size()); copy.ppEnabledExtensionNames=extensions.data();
            }
        } catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
        behavior.overallocationBehavior=VK_MEMORY_OVERALLOCATION_BEHAVIOR_ALLOWED_AMD; behavior.pNext=ci->pNext; copy.pNext=&behavior;
    }
    VkResult r=VK_ERROR_INITIALIZATION_FAILED;
    auto nextCreate=reinterpret_cast<PFN_vkCreateDevice>(nextGipa(in->handle,"vkCreateDevice"));
    if(!nextCreate) return VK_ERROR_INITIALIZATION_FAILED;
    r=nextCreate(physical,&copy,allocator,out); if(r!=VK_SUCCESS) return r;
    try {
        auto d=std::make_shared<Device>(); d->handle=*out; d->gdpa=nextGdpa;
        d->destroy=reinterpret_cast<PFN_vkDestroyDevice>(nextGdpa(*out,"vkDestroyDevice"));
        d->allocate=reinterpret_cast<PFN_vkAllocateMemory>(nextGdpa(*out,"vkAllocateMemory"));
        d->free=reinterpret_cast<PFN_vkFreeMemory>(nextGdpa(*out,"vkFreeMemory"));
        in->memoryProperties(physical,&d->memory); VkPhysicalDeviceProperties props{}; in->properties(physical,&props); d->gpu=props.deviceName; d->autoPolicy=inject;
        { std::lock_guard<std::mutex> lock(mapsMutex); devices[key(*out)]=d; }
        logf("device=%s policy=%s",d->gpu.c_str(),policy?"application-specified":inject?"allowed":"unchanged");
    } catch(const std::bad_alloc&) {
        auto destroy=reinterpret_cast<PFN_vkDestroyDevice>(nextGdpa(*out,"vkDestroyDevice")); if(destroy) destroy(*out,allocator);
        *out=VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL layerDestroyDevice(VkDevice device,const VkAllocationCallbacks* allocator) {
    auto d=findDevice(device); if(!d) return;
    { std::lock_guard<std::mutex> lock(mapsMutex); devices.erase(key(device)); }
    { std::lock_guard<std::mutex> lock(d->mutex); logf("device=%s live-local=%llu peak-local=%llu live-nonlocal=%llu peak-nonlocal=%llu allocation-failures=%llu",d->gpu.c_str(),static_cast<unsigned long long>(d->liveLocal),static_cast<unsigned long long>(d->peakLocal),static_cast<unsigned long long>(d->liveOther),static_cast<unsigned long long>(d->peakOther),static_cast<unsigned long long>(d->failures)); }
    if(d->destroy) d->destroy(device,allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL layerAllocateMemory(VkDevice device,const VkMemoryAllocateInfo* info,const VkAllocationCallbacks* allocator,VkDeviceMemory* out) {
    auto d=findDevice(device); if(!d || !d->allocate) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=d->allocate(device,info,allocator,out);
    std::unique_lock<std::mutex> lock(d->mutex);
    if(r!=VK_SUCCESS) { ++d->failures; if(verbose()) logf("allocation failed result=%d",static_cast<int>(r)); return r; }
    bool local=false; if(info->memoryTypeIndex<d->memory.memoryTypeCount) { auto heap=d->memory.memoryTypes[info->memoryTypeIndex].heapIndex; local=(d->memory.memoryHeaps[heap].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)!=0; }
    try {
        d->allocations.emplace(*out,Allocation{info->allocationSize,local});
    } catch(const std::bad_alloc&) {
        ++d->failures; lock.unlock(); d->free(device,*out,allocator); *out=VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    auto& live=local?d->liveLocal:d->liveOther; auto& peak=local?d->peakLocal:d->peakOther; live+=info->allocationSize; peak=std::max(peak,live);
    if(verbose()) logf("allocate %s bytes=%llu",local?"local":"nonlocal",static_cast<unsigned long long>(info->allocationSize));
    return r;
}
VKAPI_ATTR void VKAPI_CALL layerFreeMemory(VkDevice device,VkDeviceMemory memory,const VkAllocationCallbacks* allocator) {
    auto d=findDevice(device); if(!d || !d->free) return;
    Allocation a{}; bool found=false;
    { std::lock_guard<std::mutex> lock(d->mutex); auto i=d->allocations.find(memory);
      if(i!=d->allocations.end()) { a=i->second; auto& live=a.local?d->liveLocal:d->liveOther; live-=a.size; d->allocations.erase(i); found=true; } }
    if(found && verbose()) logf("free bytes=%llu",static_cast<unsigned long long>(a.size));
    d->free(device,memory,allocator);
}

PFN_vkVoidFunction lookup(const char* name) {
    if(!name) return nullptr;
#define MATCH(n,f) if(std::strcmp(name,n)==0) return reinterpret_cast<PFN_vkVoidFunction>(f)
    MATCH("vkGetInstanceProcAddr",layerGetInstanceProcAddr); MATCH("vkGetDeviceProcAddr",layerGetDeviceProcAddr);
    MATCH("vk_layerGetPhysicalDeviceProcAddr",layerGetPhysicalDeviceProcAddr);
    MATCH("vkCreateInstance",layerCreateInstance); MATCH("vkDestroyInstance",layerDestroyInstance);
    MATCH("vkCreateDevice",layerCreateDevice); MATCH("vkDestroyDevice",layerDestroyDevice);
    MATCH("vkAllocateMemory",layerAllocateMemory); MATCH("vkFreeMemory",layerFreeMemory);
#undef MATCH
    return nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGetInstanceProcAddr(VkInstance instance,const char* name) {
    if(auto f=lookup(name)) return f;
    auto s=findInstance(key(instance)); if(s&&s->gipa) return s->gipa(instance,name);
    if(!instance) { PFN_vkGetInstanceProcAddr next{}; { std::lock_guard<std::mutex> lock(mapsMutex); next=globalGipa; } return next?next(VK_NULL_HANDLE,name):nullptr; }
    return nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGetDeviceProcAddr(VkDevice device,const char* name) {
    if(auto f=lookup(name)) return f;
    auto d=findDevice(device); return d&&d->gdpa?d->gdpa(device,name):nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGetPhysicalDeviceProcAddr(VkInstance instance,const char* name) {
    if(auto f=lookup(name)) return f;
    auto s=findInstance(key(instance)); if(!s) return nullptr;
    return s->physProc?s->physProc(instance,name):s->gipa(instance,name);
}
} // namespace

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
    if(!v || v->sType!=LAYER_NEGOTIATE_INTERFACE_STRUCT || v->loaderLayerInterfaceVersion<1) return VK_ERROR_INITIALIZATION_FAILED;
    v->loaderLayerInterfaceVersion=std::min(v->loaderLayerInterfaceVersion,2u);
    v->pfnGetInstanceProcAddr=layerGetInstanceProcAddr; v->pfnGetDeviceProcAddr=layerGetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr=layerGetPhysicalDeviceProcAddr; return VK_SUCCESS;
}
