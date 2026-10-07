#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <memory>
#include <limits>
#include <mutex>
#include <new>
#include <type_traits>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <zstd.h>

namespace {
constexpr char kLayer[] = "VK_LAYER_NX_zvram";
template<class H> void* key(H h) { return h ? *reinterpret_cast<void**>(h) : nullptr; }
template<class H> H tokenHandle(std::uintptr_t value) {
    if constexpr (std::is_pointer_v<H>) return reinterpret_cast<H>(value);
    else return static_cast<H>(value);
}
template<class H> std::uintptr_t handleToken(H value) {
    if constexpr (std::is_pointer_v<H>) return reinterpret_cast<std::uintptr_t>(value);
    else return static_cast<std::uintptr_t>(value);
}
bool verbose() { const char* p=std::getenv("ZVRAM_VERBOSE"); return p && std::strcmp(p,"1")==0; }
void logf(const char* fmt,...) { std::fputs("[zvram] ",stderr); va_list ap; va_start(ap,fmt); std::vfprintf(stderr,fmt,ap); va_end(ap); std::fputc('\n',stderr); }

struct PhysicalMemoryView {
    VkPhysicalDeviceMemoryProperties native{};
    std::uint32_t virtualType{UINT32_MAX};
    std::uint32_t virtualHeap{UINT32_MAX};
    VkDeviceSize virtualBytes{};
    bool enabled{};
    bool radvDiscrete{};
};
struct Instance {
    VkInstance handle{};
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_GetPhysicalDeviceProcAddr physProc{};
    PFN_vkDestroyInstance destroy{};
    PFN_vkEnumerateDeviceExtensionProperties enumerateExtensions{};
    PFN_vkGetPhysicalDeviceProperties properties{};
    PFN_vkGetPhysicalDeviceMemoryProperties memoryProperties{};
    PFN_vkGetPhysicalDeviceMemoryProperties2 memoryProperties2{};
    PFN_vkGetPhysicalDeviceFeatures features{};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queueFamilies{};
    std::mutex physicalMutex;
    std::unordered_map<VkPhysicalDevice,PhysicalMemoryView> physicalViews;
};
struct Allocation { VkDeviceSize size; bool local; std::uint32_t type{}; bool bound{}; };
struct VirtualMemory {
    VkDeviceSize size{};
    VkDeviceSize residentBytes{};
    std::vector<VkDeviceMemory> children;
    std::vector<VkDeviceSize> childSizes;
    std::vector<std::uint32_t> childTypes;
    struct ColdChunk { std::vector<std::uint8_t> bytes; VkDeviceSize rawSize{}; bool compressed{}; };
    std::vector<ColdChunk> coldChunks;
    VkDeviceSize coldStoredBytes{};
    VkDeviceSize coldLogicalSize{};
    bool cold{};
    VkBuffer buffer{};
    void* token{};
    bool bound{};
    bool everBound{};
    bool deferredFree{};
};
struct ZvramSnapshotStatsNX {
    std::uint32_t structSize{};
    std::uint32_t version{};
    std::uint64_t coldLogicalBytes{};
    std::uint64_t coldStoredBytes{};
    std::uint64_t coldBudgetBytes{};
    std::uint64_t residentBytes{};
    std::uint64_t freezes{};
    std::uint64_t restores{};
    std::uint64_t failures{};
    std::int32_t lastError{};
};
struct SnapshotResources {
    PFN_vkDeviceWaitIdle deviceWaitIdle{};
    PFN_vkCreateCommandPool createCommandPool{};
    PFN_vkDestroyCommandPool destroyCommandPool{};
    PFN_vkResetCommandPool resetCommandPool{};
    PFN_vkAllocateCommandBuffers allocateCommandBuffers{};
    PFN_vkBeginCommandBuffer beginCommandBuffer{};
    PFN_vkEndCommandBuffer endCommandBuffer{};
    PFN_vkCmdCopyBuffer cmdCopyBuffer{};
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier{};
    PFN_vkQueueSubmit queueSubmit{};
    PFN_vkCreateBuffer createBuffer{};
    PFN_vkDestroyBuffer destroyBuffer{};
    PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements{};
    PFN_vkAllocateMemory allocateMemory{};
    PFN_vkFreeMemory freeMemory{};
    PFN_vkBindBufferMemory bindBufferMemory{};
    PFN_vkMapMemory mapMemory{};
    PFN_vkUnmapMemory unmapMemory{};
    VkCommandPool commandPool{};
    VkCommandBuffer commandBuffer{};
    VkBuffer stagingBuffer{};
    VkDeviceMemory stagingMemory{};
    VkDeviceSize chunkSize{32u*1024u*1024u};
    void* mapped{};
};
struct PromotedBuffer {
    VkDeviceSize size{};
    VkDeviceSize alignment{};
    VkMemoryRequirements requirements{};
    VkDeviceMemory memory{};
    bool synthetic{};
};
struct Device {
    VkDevice handle{};
    VkPhysicalDevice physical{};
    PFN_vkGetDeviceProcAddr gdpa{};
    PFN_vkDestroyDevice destroy{};
    PFN_vkAllocateMemory allocate{};
    PFN_vkFreeMemory free{};
    PFN_vkCreateBuffer createBuffer{};
    PFN_vkDestroyBuffer destroyBuffer{};
    PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements{};
    PFN_vkGetBufferMemoryRequirements2 getBufferMemoryRequirements2{};
    PFN_vkBindBufferMemory bindBufferMemory{};
    PFN_vkBindBufferMemory2 bindBufferMemory2{};
    PFN_vkQueueBindSparse queueBindSparse{};
    PFN_vkQueueWaitIdle queueWaitIdle{};
    PFN_vkGetDeviceQueue getDeviceQueue{};
    PFN_vkSetDeviceLoaderData setDeviceLoaderData{};
    std::unordered_map<VkDeviceMemory,VirtualMemory> virtualMemory;
    std::unordered_map<VkBuffer,PromotedBuffer> promotedBuffers;
    std::unordered_map<VkDeviceMemory,Allocation> allocations;
    VkQueue sparseQueue{};
    std::uint32_t virtualType{UINT32_MAX};
    std::uint32_t virtualHeap{UINT32_MAX};
    VkDeviceSize virtualBytes{};
    bool virtualEnabled{};
    std::uint64_t virtualUsage{};
    bool autoEnabled{};
    std::uint64_t idleMilliseconds{};
    std::uint64_t coldBudget{};
    std::uint64_t coldBytes{};
    std::uint64_t coldLogicalBytes{};
    std::uint64_t residentBytes{};
    std::uint64_t freezeCount{}, restoreCount{}, snapshotFailures{};
    VkResult lastSnapshotError{VK_SUCCESS};
    std::atomic<bool> stopWorker{false};
    std::condition_variable activity;
    std::chrono::steady_clock::time_point lastActivity{std::chrono::steady_clock::now()};
    std::thread snapshotWorker;
    SnapshotResources snapshot;
    VkPhysicalDeviceMemoryProperties memory{};
    std::string gpu;
    bool autoPolicy{};
    std::mutex mutex;
    // ponytail: one host queue lock per virtual device; use per-queue locks if
    // simultaneous queue host calls become a measured bottleneck.
    std::mutex queueMutex;
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
bool initSnapshotResources(Device&,std::uint32_t);
void releaseSnapshotResources(Device&);
void snapshotWorkerLoop(const std::shared_ptr<Device>&);
VkResult restoreColdLocked(VkDevice,Device&);
std::vector<std::uint32_t> backingMemoryTypes(const Device&,const VkMemoryRequirements&);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGetInstanceProcAddr(VkInstance,const char*);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGetDeviceProcAddr(VkDevice,const char*);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGetPhysicalDeviceProcAddr(VkInstance,const char*);
VKAPI_ATTR void VKAPI_CALL layerGetPhysicalDeviceMemoryProperties(VkPhysicalDevice,VkPhysicalDeviceMemoryProperties*);
VKAPI_ATTR void VKAPI_CALL layerGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice,VkPhysicalDeviceMemoryProperties2*);
VKAPI_ATTR VkResult VKAPI_CALL layerCreateBuffer(VkDevice,const VkBufferCreateInfo*,const VkAllocationCallbacks*,VkBuffer*);
VKAPI_ATTR void VKAPI_CALL layerDestroyBuffer(VkDevice,VkBuffer,const VkAllocationCallbacks*);
VKAPI_ATTR void VKAPI_CALL layerGetBufferMemoryRequirements(VkDevice,VkBuffer,VkMemoryRequirements*);
VKAPI_ATTR void VKAPI_CALL layerGetBufferMemoryRequirements2(VkDevice,const VkBufferMemoryRequirementsInfo2*,VkMemoryRequirements2*);
VKAPI_ATTR VkResult VKAPI_CALL layerBindBufferMemory(VkDevice,VkBuffer,VkDeviceMemory,VkDeviceSize);
VKAPI_ATTR VkResult VKAPI_CALL layerBindBufferMemory2(VkDevice,std::uint32_t,const VkBindBufferMemoryInfo*);
VKAPI_ATTR VkResult VKAPI_CALL layerMapMemory(VkDevice,VkDeviceMemory,VkDeviceSize,VkDeviceSize,VkMemoryMapFlags,void**);
VKAPI_ATTR VkResult VKAPI_CALL layerBindImageMemory(VkDevice,VkImage,VkDeviceMemory,VkDeviceSize);
VKAPI_ATTR VkResult VKAPI_CALL layerBindImageMemory2(VkDevice,std::uint32_t,const VkBindImageMemoryInfo*);
VKAPI_ATTR void VKAPI_CALL layerGetDeviceMemoryCommitment(VkDevice,VkDeviceMemory,VkDeviceSize*);
VKAPI_ATTR VkResult VKAPI_CALL layerGetSnapshotStats(VkDevice,ZvramSnapshotStatsNX*);
VKAPI_ATTR VkResult VKAPI_CALL layerAllocateMemory(VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory*);
VKAPI_ATTR void VKAPI_CALL layerFreeMemory(VkDevice,VkDeviceMemory,const VkAllocationCallbacks*);

std::uint64_t configuredVirtualBytes() {
    const char* value=std::getenv("ZVRAM_VULKAN_VIRTUAL_MIB");
    if(!value || !*value) return 0;
    char* end=nullptr; errno=0;
    const unsigned long long mib=std::strtoull(value,&end,10);
    if(errno || end==value || *end || mib==0 || mib>std::numeric_limits<VkDeviceSize>::max()/(1024ull*1024ull)) return 0;
    return mib*1024ull*1024ull;
}
std::uint64_t positiveEnv(const char* name,std::uint64_t max=std::numeric_limits<std::uint64_t>::max()) {
    const char* value=std::getenv(name); if(!value || !*value) return 0;
    char* end=nullptr; errno=0; const auto parsed=std::strtoull(value,&end,10);
    if(errno || end==value || *end || parsed==0 || parsed>max) return 0;
    return parsed;
}
bool snapshotConfig(const Device& d,std::uint64_t& idleMs,std::uint64_t& coldBudget) {
    idleMs=positiveEnv("ZVRAM_VULKAN_AUTO_IDLE_MS",60000);
    const auto coldMiB=positiveEnv("ZVRAM_VULKAN_COLD_MIB");
    if(!idleMs || !coldMiB || coldMiB>std::numeric_limits<std::uint64_t>::max()/(1024ull*1024ull)) return false;
    coldBudget=coldMiB*1024ull*1024ull;
    return coldBudget<=d.virtualBytes && coldBudget<=std::numeric_limits<std::size_t>::max();
}
PhysicalMemoryView physicalView(const std::shared_ptr<Instance>& instance,VkPhysicalDevice physical) {
    std::lock_guard<std::mutex> lock(instance->physicalMutex);
    try {
    auto found=instance->physicalViews.find(physical);
    if(found!=instance->physicalViews.end()) return found->second;
    PhysicalMemoryView view{};
    if(!instance->memoryProperties || !instance->properties || !instance->features || !instance->queueFamilies) return view;
    instance->memoryProperties(physical,&view.native);
    VkPhysicalDeviceProperties props{}; instance->properties(physical,&props);
    const auto configured=configuredVirtualBytes();
    if(!configured || props.deviceType!=VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
       std::strstr(props.deviceName,"RADV")==nullptr ||
       view.native.memoryTypeCount>=VK_MAX_MEMORY_TYPES || view.native.memoryHeapCount>=VK_MAX_MEMORY_HEAPS)
        { instance->physicalViews.emplace(physical,view); return view; }
    VkPhysicalDeviceFeatures features{}; instance->features(physical,&features);
    if(!features.sparseBinding) { instance->physicalViews.emplace(physical,view); return view; }
    std::uint32_t count=0; instance->queueFamilies(physical,&count,nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    instance->queueFamilies(physical,&count,queues.data());
    if(std::none_of(queues.begin(),queues.end(),[](const auto& q){return (q.queueFlags&VK_QUEUE_SPARSE_BINDING_BIT)!=0;})) { instance->physicalViews.emplace(physical,view); return view; }
    view.virtualType=view.native.memoryTypeCount;
    view.virtualHeap=view.native.memoryHeapCount;
    view.virtualBytes=configured;
    view.enabled=true; view.radvDiscrete=true;
    instance->physicalViews.emplace(physical,view);
    return view;
    } catch(const std::bad_alloc&) { return {}; }
}
void exposeVirtualMemory(const PhysicalMemoryView& view,VkPhysicalDeviceMemoryProperties* props) {
    if(!view.enabled || !props) return;
    props->memoryHeaps[view.virtualHeap]={view.virtualBytes,VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    props->memoryTypes[view.virtualType]={VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,view.virtualHeap};
    props->memoryHeapCount=view.virtualHeap+1;
    props->memoryTypeCount=view.virtualType+1;
}

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
        s->memoryProperties2=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(next(*out,"vkGetPhysicalDeviceMemoryProperties2"));
        if(!s->memoryProperties2) s->memoryProperties2=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(next(*out,"vkGetPhysicalDeviceMemoryProperties2KHR"));
        s->features=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(next(*out,"vkGetPhysicalDeviceFeatures"));
        s->queueFamilies=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(next(*out,"vkGetPhysicalDeviceQueueFamilyProperties"));
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
VKAPI_ATTR void VKAPI_CALL layerGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physical,VkPhysicalDeviceMemoryProperties* out) {
    auto in=findInstance(key(physical));
    if(!in || !in->memoryProperties) return;
    in->memoryProperties(physical,out);
    exposeVirtualMemory(physicalView(in,physical),out);
}
VKAPI_ATTR void VKAPI_CALL layerGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physical,VkPhysicalDeviceMemoryProperties2* out) {
    auto in=findInstance(key(physical));
    if(!in || !in->memoryProperties2) return;
    in->memoryProperties2(physical,out);
    auto view=physicalView(in,physical);
    exposeVirtualMemory(view,out?&out->memoryProperties:nullptr);
#ifdef VK_EXT_memory_budget
    if(view.enabled && out) for(auto* p=static_cast<VkBaseOutStructure*>(out->pNext);p;p=p->pNext) {
        if(p->sType!=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) continue;
        auto* budget=reinterpret_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(p);
        budget->heapBudget[view.virtualHeap]=view.virtualBytes;
        std::uint64_t usage=0;
        { std::lock_guard<std::mutex> lock(mapsMutex);
          for(const auto& pair:devices) if(pair.second->physical==physical) {
              std::lock_guard<std::mutex> deviceLock(pair.second->mutex); usage+=pair.second->virtualUsage;
          } }
        budget->heapUsage[view.virtualHeap]=usage;
        break;
    }
#endif
}
VKAPI_ATTR VkResult VKAPI_CALL layerCreateDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo* ci,const VkAllocationCallbacks* allocator,VkDevice* out) {
    if(!ci || !out) return VK_ERROR_INITIALIZATION_FAILED;
    auto in=findInstance(key(physical)); if(!in) return VK_ERROR_INITIALIZATION_FAILED;
    auto view=physicalView(in,physical);
    auto* link=deviceLink(ci); if(!link || !link->u.pLayerInfo) { logf("missing loader device link information"); return VK_ERROR_INITIALIZATION_FAILED; }
    PFN_vkSetDeviceLoaderData setDeviceLoaderData=nullptr;
    for(auto* p=static_cast<const VkBaseInStructure*>(ci->pNext);p;p=p->pNext)
        if(p->sType==VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            auto* loader=reinterpret_cast<const VkLayerDeviceCreateInfo*>(p);
            if(loader->function==VK_LOADER_DATA_CALLBACK) setDeviceLoaderData=loader->u.pfnSetDeviceLoaderData;
        }
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
    std::uint32_t sparseFamily=UINT32_MAX, familyCount=0, sparseQueueFlags=0;
    if(view.enabled && in->queueFamilies) {
        in->queueFamilies(physical,&familyCount,nullptr);
        std::vector<VkQueueFamilyProperties> queueProps(familyCount);
        in->queueFamilies(physical,&familyCount,queueProps.data());
        for(std::uint32_t i=0;i<familyCount;i++) {
            if((queueProps[i].queueFlags&VK_QUEUE_SPARSE_BINDING_BIT)==0) continue;
            for(std::uint32_t q=0;q<ci->queueCreateInfoCount;q++)
                if(ci->pQueueCreateInfos[q].queueFamilyIndex==i && ci->pQueueCreateInfos[q].queueCount) { sparseFamily=i; sparseQueueFlags=queueProps[i].queueFlags; break; }
            if(sparseFamily!=UINT32_MAX) break;
        }
    }
    bool virtualEnabled=view.enabled && sparseFamily!=UINT32_MAX;
    const bool singleTransferQueue=ci->queueCreateInfoCount==1 && ci->pQueueCreateInfos &&
        ci->pQueueCreateInfos[0].queueCount==1 && ci->pQueueCreateInfos[0].flags==0 &&
        (sparseQueueFlags&(VK_QUEUE_SPARSE_BINDING_BIT|VK_QUEUE_TRANSFER_BIT))==
            (VK_QUEUE_SPARSE_BINDING_BIT|VK_QUEUE_TRANSFER_BIT);
    VkPhysicalDeviceFeatures injectedFeatures{};
    VkPhysicalDeviceFeatures2 injectedFeatures2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    const auto* featureChain=static_cast<const VkBaseInStructure*>(ci->pNext);
    bool hasFeatures2=false;
    for(auto* p=featureChain;p;p=p->pNext) if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) { hasFeatures2=true; break; }
    const bool features2AtHead=ci->pNext && static_cast<const VkBaseInStructure*>(ci->pNext)->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    if(hasFeatures2 && !features2AtHead) virtualEnabled=false;
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
    if(virtualEnabled) {
        if(hasFeatures2) {
            auto* f2=const_cast<VkPhysicalDeviceFeatures2*>(reinterpret_cast<const VkPhysicalDeviceFeatures2*>(ci->pNext));
            injectedFeatures2=*f2; injectedFeatures2.features.sparseBinding=VK_TRUE;
            if(copy.pNext==ci->pNext) copy.pNext=&injectedFeatures2;
            else behavior.pNext=&injectedFeatures2;
        } else {
            if(ci->pEnabledFeatures) injectedFeatures=*ci->pEnabledFeatures;
            injectedFeatures.sparseBinding=VK_TRUE;
            copy.pEnabledFeatures=&injectedFeatures;
        }
    }
    VkResult r=VK_ERROR_INITIALIZATION_FAILED;
    auto nextCreate=reinterpret_cast<PFN_vkCreateDevice>(nextGipa(in->handle,"vkCreateDevice"));
    if(!nextCreate) return VK_ERROR_INITIALIZATION_FAILED;
    r=nextCreate(physical,&copy,allocator,out); if(r!=VK_SUCCESS) return r;
    try {
        auto d=std::make_shared<Device>(); d->handle=*out; d->physical=physical; d->gdpa=nextGdpa; d->setDeviceLoaderData=setDeviceLoaderData;
        d->destroy=reinterpret_cast<PFN_vkDestroyDevice>(nextGdpa(*out,"vkDestroyDevice"));
        d->allocate=reinterpret_cast<PFN_vkAllocateMemory>(nextGdpa(*out,"vkAllocateMemory"));
        d->free=reinterpret_cast<PFN_vkFreeMemory>(nextGdpa(*out,"vkFreeMemory"));
        d->createBuffer=reinterpret_cast<PFN_vkCreateBuffer>(nextGdpa(*out,"vkCreateBuffer"));
        d->destroyBuffer=reinterpret_cast<PFN_vkDestroyBuffer>(nextGdpa(*out,"vkDestroyBuffer"));
        d->getBufferMemoryRequirements=reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(nextGdpa(*out,"vkGetBufferMemoryRequirements"));
        d->getBufferMemoryRequirements2=reinterpret_cast<PFN_vkGetBufferMemoryRequirements2>(nextGdpa(*out,"vkGetBufferMemoryRequirements2"));
        if(!d->getBufferMemoryRequirements2) d->getBufferMemoryRequirements2=reinterpret_cast<PFN_vkGetBufferMemoryRequirements2>(nextGdpa(*out,"vkGetBufferMemoryRequirements2KHR"));
        d->bindBufferMemory=reinterpret_cast<PFN_vkBindBufferMemory>(nextGdpa(*out,"vkBindBufferMemory"));
        d->bindBufferMemory2=reinterpret_cast<PFN_vkBindBufferMemory2>(nextGdpa(*out,"vkBindBufferMemory2"));
        if(!d->bindBufferMemory2) d->bindBufferMemory2=reinterpret_cast<PFN_vkBindBufferMemory2>(nextGdpa(*out,"vkBindBufferMemory2KHR"));
        d->queueBindSparse=reinterpret_cast<PFN_vkQueueBindSparse>(nextGdpa(*out,"vkQueueBindSparse"));
        d->queueWaitIdle=reinterpret_cast<PFN_vkQueueWaitIdle>(nextGdpa(*out,"vkQueueWaitIdle"));
        d->getDeviceQueue=reinterpret_cast<PFN_vkGetDeviceQueue>(nextGdpa(*out,"vkGetDeviceQueue"));
        d->virtualEnabled=virtualEnabled; d->virtualType=view.virtualType; d->virtualHeap=view.virtualHeap; d->virtualBytes=view.virtualBytes;
        if(virtualEnabled && d->getDeviceQueue) d->getDeviceQueue(*out,sparseFamily,0,&d->sparseQueue);
        in->memoryProperties(physical,&d->memory); VkPhysicalDeviceProperties props{}; in->properties(physical,&props); d->gpu=props.deviceName; d->autoPolicy=inject;
        { std::lock_guard<std::mutex> lock(mapsMutex); devices[key(*out)]=d; }
        const bool snapshotRequested=std::getenv("ZVRAM_VULKAN_AUTO_IDLE_MS") || std::getenv("ZVRAM_VULKAN_COLD_MIB");
        if(snapshotRequested) {
            if(!singleTransferQueue || !virtualEnabled || !d->sparseQueue)
                logf("automatic Vulkan snapshots disabled: requires one created sparse+transfer queue and virtual-memory opt-in");
            else if(!snapshotConfig(*d,d->idleMilliseconds,d->coldBudget))
                logf("automatic Vulkan snapshots disabled: invalid idle interval or cold budget");
            else if(!initSnapshotResources(*d,sparseFamily))
                { releaseSnapshotResources(*d); logf("automatic Vulkan snapshots disabled: private staging resources unavailable"); }
            else {
                d->autoEnabled=true;
                try { d->snapshotWorker=std::thread(snapshotWorkerLoop,d); }
                catch(...) { d->autoEnabled=false; releaseSnapshotResources(*d); }
                if(d->autoEnabled) logf("automatic Vulkan snapshots enabled idle-ms=%llu cold-budget=%llu",static_cast<unsigned long long>(d->idleMilliseconds),static_cast<unsigned long long>(d->coldBudget));
                else logf("automatic Vulkan snapshots disabled: worker creation failed");
            }
        }
        logf("device=%s policy=%s",d->gpu.c_str(),policy?"application-specified":inject?"allowed":"unchanged");
    } catch(const std::bad_alloc&) {
        auto destroy=reinterpret_cast<PFN_vkDestroyDevice>(nextGdpa(*out,"vkDestroyDevice")); if(destroy) destroy(*out,allocator);
        *out=VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return r;
}
VkResult bindSparseLocked(VkDevice d,Device& state,VkBuffer buffer,const VkSparseMemoryBind* binds,std::uint32_t count) {
    if(!state.sparseQueue || !state.queueBindSparse || !state.queueWaitIdle) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkSparseBufferMemoryBindInfo bufferInfo{}; bufferInfo.buffer=buffer; bufferInfo.bindCount=count; bufferInfo.pBinds=binds;
    VkBindSparseInfo info{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO}; info.bufferBindCount=1; info.pBufferBinds=&bufferInfo;
    VkResult r=state.queueBindSparse(state.sparseQueue,1,&info,VK_NULL_HANDLE);
    if(r!=VK_SUCCESS) return r;
    return state.queueWaitIdle(state.sparseQueue);
}
VkResult bindSparse(VkDevice d,Device& state,VkBuffer buffer,const VkSparseMemoryBind* binds,std::uint32_t count) {
    std::lock_guard<std::mutex> queueLock(state.queueMutex);
    return bindSparseLocked(d,state,buffer,binds,count);
}
void releaseChildren(Device& d,VirtualMemory& memory) {
    for(auto child:memory.children) if(child) d.free(d.handle,child,nullptr);
    memory.children.clear(); memory.childSizes.clear(); memory.childTypes.clear(); memory.residentBytes=0;
}
bool initSnapshotResources(Device& d,std::uint32_t family) {
    auto get=[&](const char* name){return d.gdpa(d.handle,name);};
    auto& s=d.snapshot;
    s.deviceWaitIdle=reinterpret_cast<PFN_vkDeviceWaitIdle>(get("vkDeviceWaitIdle"));
    s.createCommandPool=reinterpret_cast<PFN_vkCreateCommandPool>(get("vkCreateCommandPool"));
    s.destroyCommandPool=reinterpret_cast<PFN_vkDestroyCommandPool>(get("vkDestroyCommandPool"));
    s.resetCommandPool=reinterpret_cast<PFN_vkResetCommandPool>(get("vkResetCommandPool"));
    s.allocateCommandBuffers=reinterpret_cast<PFN_vkAllocateCommandBuffers>(get("vkAllocateCommandBuffers"));
    s.beginCommandBuffer=reinterpret_cast<PFN_vkBeginCommandBuffer>(get("vkBeginCommandBuffer"));
    s.endCommandBuffer=reinterpret_cast<PFN_vkEndCommandBuffer>(get("vkEndCommandBuffer"));
    s.cmdCopyBuffer=reinterpret_cast<PFN_vkCmdCopyBuffer>(get("vkCmdCopyBuffer"));
    s.cmdPipelineBarrier=reinterpret_cast<PFN_vkCmdPipelineBarrier>(get("vkCmdPipelineBarrier"));
    s.queueSubmit=reinterpret_cast<PFN_vkQueueSubmit>(get("vkQueueSubmit"));
    s.createBuffer=reinterpret_cast<PFN_vkCreateBuffer>(get("vkCreateBuffer"));
    s.destroyBuffer=reinterpret_cast<PFN_vkDestroyBuffer>(get("vkDestroyBuffer"));
    s.getBufferMemoryRequirements=reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(get("vkGetBufferMemoryRequirements"));
    s.allocateMemory=reinterpret_cast<PFN_vkAllocateMemory>(get("vkAllocateMemory"));
    s.freeMemory=reinterpret_cast<PFN_vkFreeMemory>(get("vkFreeMemory"));
    s.bindBufferMemory=reinterpret_cast<PFN_vkBindBufferMemory>(get("vkBindBufferMemory"));
    s.mapMemory=reinterpret_cast<PFN_vkMapMemory>(get("vkMapMemory"));
    s.unmapMemory=reinterpret_cast<PFN_vkUnmapMemory>(get("vkUnmapMemory"));
    if(!s.deviceWaitIdle || !s.createCommandPool || !s.destroyCommandPool || !s.resetCommandPool ||
       !s.allocateCommandBuffers || !s.beginCommandBuffer || !s.endCommandBuffer || !s.cmdCopyBuffer ||
       !s.cmdPipelineBarrier || !s.queueSubmit || !s.createBuffer || !s.destroyBuffer ||
       !s.getBufferMemoryRequirements || !s.allocateMemory || !s.freeMemory || !s.bindBufferMemory ||
       !s.mapMemory || !s.unmapMemory || !d.queueWaitIdle || !d.setDeviceLoaderData) return false;
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pool.queueFamilyIndex=family;
    if(s.createCommandPool(d.handle,&pool,nullptr,&s.commandPool)!=VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command.commandPool=s.commandPool; command.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; command.commandBufferCount=1;
    if(s.allocateCommandBuffers(d.handle,&command,&s.commandBuffer)!=VK_SUCCESS) return false;
    if(d.setDeviceLoaderData(d.handle,s.commandBuffer)!=VK_SUCCESS) return false;
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; buffer.size=s.chunkSize;
    buffer.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    if(s.createBuffer(d.handle,&buffer,nullptr,&s.stagingBuffer)!=VK_SUCCESS) return false;
    VkMemoryRequirements req{}; s.getBufferMemoryRequirements(d.handle,s.stagingBuffer,&req);
    std::uint32_t type=UINT32_MAX; int bestScore=-1;
    for(std::uint32_t i=0;i<d.memory.memoryTypeCount;i++) {
        if(!(req.memoryTypeBits&(1u<<i))) continue;
        const auto flags=d.memory.memoryTypes[i].propertyFlags;
        if((flags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))!=
           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) continue;
        const auto heap=d.memory.memoryTypes[i].heapIndex;
        const bool local=heap<d.memory.memoryHeapCount && (d.memory.memoryHeaps[heap].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
        const bool cached=(flags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)!=0;
        const int score=!local&&cached?4:cached?3:!local?2:1;
        if(score>bestScore) { type=i; bestScore=score; }
    }
    if(type==UINT32_MAX) return false;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize=req.size; allocation.memoryTypeIndex=type;
    if(s.allocateMemory(d.handle,&allocation,nullptr,&s.stagingMemory)!=VK_SUCCESS) return false;
    if(s.bindBufferMemory(d.handle,s.stagingBuffer,s.stagingMemory,0)!=VK_SUCCESS) return false;
    return s.mapMemory(d.handle,s.stagingMemory,0,s.chunkSize,0,&s.mapped)==VK_SUCCESS;
}
void releaseSnapshotResources(Device& d) {
    auto& s=d.snapshot;
    if(s.mapped && s.unmapMemory) s.unmapMemory(d.handle,s.stagingMemory);
    if(s.stagingBuffer && s.destroyBuffer) s.destroyBuffer(d.handle,s.stagingBuffer,nullptr);
    if(s.stagingMemory && s.freeMemory) s.freeMemory(d.handle,s.stagingMemory,nullptr);
    if(s.commandPool && s.destroyCommandPool) s.destroyCommandPool(d.handle,s.commandPool,nullptr);
    s={};
}
VkResult copyChunkLocked(Device& d,VkBuffer source,VkBuffer destination,VkDeviceSize sourceOffset,
                         VkDeviceSize destinationOffset,VkDeviceSize bytes,bool restoring) {
    auto& s=d.snapshot;
    VkResult r=s.resetCommandPool(d.handle,s.commandPool,0); if(r!=VK_SUCCESS) return r;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r=s.beginCommandBuffer(s.commandBuffer,&begin); if(r!=VK_SUCCESS) return r;
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask=restoring?VK_ACCESS_HOST_WRITE_BIT:VK_ACCESS_MEMORY_WRITE_BIT;
    before.dstAccessMask=restoring?VK_ACCESS_TRANSFER_READ_BIT:VK_ACCESS_TRANSFER_READ_BIT;
    s.cmdPipelineBarrier(s.commandBuffer,restoring?VK_PIPELINE_STAGE_HOST_BIT:VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&before,0,nullptr,0,nullptr);
    VkBufferCopy region{}; region.srcOffset=sourceOffset; region.dstOffset=destinationOffset; region.size=bytes;
    s.cmdCopyBuffer(s.commandBuffer,source,destination,1,&region);
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask=restoring?(VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT):VK_ACCESS_HOST_READ_BIT;
    s.cmdPipelineBarrier(s.commandBuffer,VK_PIPELINE_STAGE_TRANSFER_BIT,
                         restoring?VK_PIPELINE_STAGE_ALL_COMMANDS_BIT:VK_PIPELINE_STAGE_HOST_BIT,
                         0,1,&after,0,nullptr,0,nullptr);
    r=s.endCommandBuffer(s.commandBuffer); if(r!=VK_SUCCESS) return r;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&s.commandBuffer;
    r=s.queueSubmit(d.sparseQueue,1,&submit,VK_NULL_HANDLE);
    return r==VK_SUCCESS?d.queueWaitIdle(d.sparseQueue):r;
}
VkResult restoreColdLocked(VkDevice device,Device& d) {
    auto& s=d.snapshot;
    for(auto& pair:d.virtualMemory) {
        auto& memory=pair.second;
        if(!memory.cold || !memory.buffer) continue;
        auto bi=d.promotedBuffers.find(memory.buffer);
        if(bi==d.promotedBuffers.end()) return VK_ERROR_FEATURE_NOT_PRESENT;
        VkResult r=s.deviceWaitIdle(device); if(r!=VK_SUCCESS) return r;
        if(memory.children.empty()) {
            std::vector<std::uint32_t> types;
            try { types=backingMemoryTypes(d,bi->second.requirements); }
            catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
            if(types.empty()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            const VkDeviceSize chunk=std::max<VkDeviceSize>(bi->second.requirements.alignment,
                (256u*1024u*1024u/bi->second.requirements.alignment)*bi->second.requirements.alignment);
            std::vector<VkSparseMemoryBind> binds;
            try {
                const auto segments=(bi->second.requirements.size+chunk-1)/chunk;
                memory.children.reserve(static_cast<std::size_t>(segments)); memory.childSizes.reserve(static_cast<std::size_t>(segments));
                memory.childTypes.reserve(static_cast<std::size_t>(segments)); binds.reserve(static_cast<std::size_t>(segments));
                VkDeviceSize offset=0;
                while(offset<bi->second.requirements.size) {
                    const auto amount=std::min(chunk,bi->second.requirements.size-offset);
                    VkDeviceMemory child{}; std::uint32_t used=UINT32_MAX; r=VK_ERROR_OUT_OF_DEVICE_MEMORY;
                    for(auto type:types) { VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=amount; ai.memoryTypeIndex=type;
                        r=d.allocate(device,&ai,nullptr,&child); if(r==VK_SUCCESS) { used=type; break; } }
                    if(r!=VK_SUCCESS) { releaseChildren(d,memory); return r; }
                    memory.children.push_back(child); memory.childSizes.push_back(amount); memory.childTypes.push_back(used);
                    VkSparseMemoryBind bind{}; bind.resourceOffset=offset; bind.size=amount; bind.memory=child; binds.push_back(bind); offset+=amount;
                }
            } catch(const std::bad_alloc&) { releaseChildren(d,memory); return VK_ERROR_OUT_OF_HOST_MEMORY; }
            r=bindSparseLocked(device,d,memory.buffer,binds.data(),static_cast<std::uint32_t>(binds.size()));
            if(r!=VK_SUCCESS) { releaseChildren(d,memory); return r; }
            memory.bound=true; memory.residentBytes=bi->second.requirements.size; d.residentBytes+=memory.residentBytes;
        }
        VkDeviceSize offset=0;
        for(const auto& chunk:memory.coldChunks) {
            auto* mapped=static_cast<std::uint8_t*>(s.mapped);
            if(chunk.compressed) {
                const auto size=ZSTD_decompress(mapped,static_cast<std::size_t>(s.chunkSize),chunk.bytes.data(),chunk.bytes.size());
                if(ZSTD_isError(size) || size!=chunk.rawSize) { r=VK_ERROR_UNKNOWN; break; }
            } else std::memcpy(mapped,chunk.bytes.data(),static_cast<std::size_t>(chunk.rawSize));
            r=copyChunkLocked(d,s.stagingBuffer,memory.buffer,0,offset,chunk.rawSize,true);
            if(r!=VK_SUCCESS) break; offset+=chunk.rawSize;
        }
        if(r!=VK_SUCCESS) {
            std::vector<VkSparseMemoryBind> unbinds;
            try { unbinds.resize(memory.children.size()); } catch(...) { return r; }
            VkDeviceSize at=0; for(std::size_t i=0;i<unbinds.size();i++) { unbinds[i].resourceOffset=at; unbinds[i].size=memory.childSizes[i]; at+=memory.childSizes[i]; }
            if(bindSparseLocked(device,d,memory.buffer,unbinds.data(),static_cast<std::uint32_t>(unbinds.size()))==VK_SUCCESS) {
                d.residentBytes-=memory.residentBytes; memory.bound=false; releaseChildren(d,memory);
            }
            return r;
        }
        d.coldBytes-=memory.coldStoredBytes; d.coldLogicalBytes-=memory.coldLogicalSize;
        memory.coldChunks.clear(); memory.coldStoredBytes=0; memory.cold=false;
        memory.coldLogicalSize=0;
        memory.bound=true;
        ++d.restoreCount;
    }
    return VK_SUCCESS;
}
void snapshotWorkerLoop(const std::shared_ptr<Device>& shared) {
    auto& d=*shared;
    std::unique_lock<std::mutex> lock(d.mutex);
    while(!d.stopWorker.load()) {
        const auto deadline=d.lastActivity+std::chrono::milliseconds(d.idleMilliseconds);
        d.activity.wait_until(lock,deadline);
        if(d.stopWorker.load()) break;
        if(std::chrono::steady_clock::now()<d.lastActivity+std::chrono::milliseconds(d.idleMilliseconds)) continue;
        std::unique_lock<std::mutex> queueLock(d.queueMutex);
        const auto start=std::chrono::steady_clock::now();
        VkResult r=d.snapshot.deviceWaitIdle(d.handle);
        const auto waited=std::chrono::steady_clock::now()-start;
        if(r!=VK_SUCCESS || waited>std::chrono::milliseconds(20)) {
            d.lastActivity=std::chrono::steady_clock::now(); if(r!=VK_SUCCESS) { ++d.snapshotFailures; d.lastSnapshotError=r; }
            queueLock.unlock(); continue;
        }
        for(auto& pair:d.virtualMemory) {
            auto& memory=pair.second;
            if(!memory.bound || !memory.buffer || memory.cold || memory.children.empty()) continue;
            auto promoted=d.promotedBuffers.find(memory.buffer);
            if(promoted==d.promotedBuffers.end()) continue;
            const auto dataSize=promoted->second.size;
            std::vector<VirtualMemory::ColdChunk> chunks;
            VkDeviceSize stored=0;
            bool okay=true;
            try {
                chunks.reserve(static_cast<std::size_t>((dataSize+d.snapshot.chunkSize-1)/d.snapshot.chunkSize));
                for(VkDeviceSize offset=0;offset<dataSize;offset+=d.snapshot.chunkSize) {
                    const auto amount=std::min(d.snapshot.chunkSize,dataSize-offset);
                    r=copyChunkLocked(d,memory.buffer,d.snapshot.stagingBuffer,offset,0,amount,false);
                    if(r!=VK_SUCCESS) { okay=false; break; }
                    VirtualMemory::ColdChunk chunk; chunk.rawSize=amount;
                    std::vector<std::uint8_t> encoded(ZSTD_compressBound(static_cast<std::size_t>(amount)));
                    const auto compressed=ZSTD_compress(encoded.data(),encoded.size(),d.snapshot.mapped,static_cast<std::size_t>(amount),1);
                    if(!ZSTD_isError(compressed) && compressed<amount) {
                        chunk.bytes.resize(compressed); std::memcpy(chunk.bytes.data(),encoded.data(),compressed); chunk.compressed=true;
                    } else {
                        chunk.bytes.resize(static_cast<std::size_t>(amount)); std::memcpy(chunk.bytes.data(),d.snapshot.mapped,static_cast<std::size_t>(amount));
                    }
                    const auto remaining=d.coldBudget-d.coldBytes;
                    if(stored>remaining || chunk.bytes.size()>remaining-stored) { okay=false; break; }
                    stored+=chunk.bytes.size(); chunks.push_back(std::move(chunk));
                }
            } catch(const std::bad_alloc&) { okay=false; r=VK_ERROR_OUT_OF_HOST_MEMORY; }
            if(!okay || chunks.size()!=(dataSize+d.snapshot.chunkSize-1)/d.snapshot.chunkSize) {
                ++d.snapshotFailures; d.lastSnapshotError=okay?VK_ERROR_OUT_OF_DEVICE_MEMORY:(r==VK_SUCCESS?VK_ERROR_OUT_OF_DEVICE_MEMORY:r); continue;
            }
            std::vector<VkSparseMemoryBind> unbinds;
            try { unbinds.resize(memory.children.size()); } catch(const std::bad_alloc&) { ++d.snapshotFailures; d.lastSnapshotError=VK_ERROR_OUT_OF_HOST_MEMORY; continue; }
            VkDeviceSize at=0; for(std::size_t i=0;i<unbinds.size();i++) { unbinds[i].resourceOffset=at; unbinds[i].size=memory.childSizes[i]; at+=memory.childSizes[i]; }
            r=bindSparseLocked(d.handle,d,memory.buffer,unbinds.data(),static_cast<std::uint32_t>(unbinds.size()));
            if(r!=VK_SUCCESS) { ++d.snapshotFailures; d.lastSnapshotError=r; continue; }
            memory.coldLogicalSize=dataSize;
            d.residentBytes-=memory.residentBytes; memory.bound=false; releaseChildren(d,memory);
            memory.coldChunks=std::move(chunks); memory.coldStoredBytes=stored; memory.cold=true;
            d.coldBytes+=stored; d.coldLogicalBytes+=memory.coldLogicalSize; ++d.freezeCount;
            logf("snapshot cold bytes=%llu stored=%llu",static_cast<unsigned long long>(memory.coldLogicalSize),static_cast<unsigned long long>(stored));
        }
        d.lastActivity=std::chrono::steady_clock::now();
        queueLock.unlock();
    }
}
bool eligibleBuffer(const Device& d,const VkBufferCreateInfo* ci) {
    constexpr VkBufferUsageFlags allowed=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return d.virtualEnabled && ci && !ci->pNext && ci->flags==0 &&
        ci->size>=1024u*1024u && ci->size%4u==0 &&
        (ci->usage&VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)!=0 && (ci->usage&~allowed)==0;
}
bool priorityOnlyChain(const void* chain) {
#ifdef VK_EXT_memory_priority
    for(auto* p=static_cast<const VkBaseInStructure*>(chain);p;p=p->pNext)
        if(p->sType!=VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT) return false;
    return true;
#else
    return chain==nullptr;
#endif
}
std::vector<std::uint32_t> backingMemoryTypes(const Device& d,const VkMemoryRequirements& req) {
    std::vector<std::uint32_t> types;
    for(std::uint32_t i=0;i<d.memory.memoryTypeCount;i++) {
        if((req.memoryTypeBits&(1u<<i))==0) continue;
        const auto heap=d.memory.memoryTypes[i].heapIndex;
        if(heap>=d.memory.memoryHeapCount) continue;
        if(d.memory.memoryHeaps[heap].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) types.push_back(i);
    }
    for(std::uint32_t i=0;i<d.memory.memoryTypeCount;i++) {
        if((req.memoryTypeBits&(1u<<i))==0) continue;
        const auto heap=d.memory.memoryTypes[i].heapIndex;
        if(heap<d.memory.memoryHeapCount && (d.memory.memoryHeaps[heap].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)==0) types.push_back(i);
    }
    return types;
}
VkResult bindPromoted(VkDevice device,const std::shared_ptr<Device>& d,VkBuffer buffer,
                      VkDeviceMemory memory,VkDeviceSize memoryOffset) {
    bool missingRequirements=false;
    { std::lock_guard<std::mutex> lock(d->mutex); auto i=d->promotedBuffers.find(buffer); missingRequirements=i!=d->promotedBuffers.end() && i->second.requirements.size==0; }
    if(missingRequirements && d->getBufferMemoryRequirements) {
        VkMemoryRequirements req{}; d->getBufferMemoryRequirements(device,buffer,&req);
        std::lock_guard<std::mutex> lock(d->mutex); auto i=d->promotedBuffers.find(buffer); if(i!=d->promotedBuffers.end()) i->second.requirements=req;
    }
    std::lock_guard<std::mutex> lock(d->mutex);
    auto bi=d->promotedBuffers.find(buffer);
    if(bi==d->promotedBuffers.end()) return VK_ERROR_FEATURE_NOT_PRESENT;
    auto& promoted=bi->second;
    if(promoted.memory || !promoted.requirements.size ||
       promoted.requirements.size>d->virtualBytes) return VK_ERROR_FEATURE_NOT_PRESENT;
    auto vi=d->virtualMemory.find(memory);
    if(vi==d->virtualMemory.end()) {
        auto ai=d->allocations.find(memory);
        if(ai==d->allocations.end() || ai->second.bound || ai->second.type>=32 ||
           (promoted.requirements.memoryTypeBits&(1u<<ai->second.type))==0 ||
           promoted.requirements.alignment==0 || memoryOffset%promoted.requirements.alignment!=0 ||
           memoryOffset>ai->second.size || promoted.requirements.size>ai->second.size-memoryOffset)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        VkSparseMemoryBind bind{}; bind.size=promoted.requirements.size; bind.memory=memory; bind.memoryOffset=memoryOffset;
        VkResult r=bindSparse(device,*d,buffer,&bind,1);
        if(r==VK_SUCCESS) { ai->second.bound=true; promoted.memory=memory; promoted.synthetic=false; }
        return r;
    }
    auto& virtualMem=vi->second;
    if(virtualMem.bound || virtualMem.everBound || virtualMem.deferredFree || virtualMem.size!=promoted.requirements.size ||
       promoted.requirements.alignment==0 || memoryOffset!=0) return VK_ERROR_FEATURE_NOT_PRESENT;
    std::vector<std::uint32_t> types;
    try { types=backingMemoryTypes(*d,promoted.requirements); }
    catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    if(types.empty()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkDeviceSize chunk=256u*1024u*1024u;
    chunk-=chunk%promoted.requirements.alignment;
    if(!chunk) return VK_ERROR_FEATURE_NOT_PRESENT;
    std::vector<VkSparseMemoryBind> binds;
    std::vector<VkDeviceMemory> children;
    std::vector<VkDeviceSize> childSizes;
    std::vector<std::uint32_t> childTypes;
    try {
        const VkDeviceSize segmentCount=(promoted.requirements.size+chunk-1)/chunk;
        if(segmentCount>UINT32_MAX) return VK_ERROR_OUT_OF_HOST_MEMORY;
        binds.reserve(static_cast<std::size_t>(segmentCount));
        children.reserve(static_cast<std::size_t>(segmentCount));
        childSizes.reserve(static_cast<std::size_t>(segmentCount));
        childTypes.reserve(static_cast<std::size_t>(segmentCount));
        VkDeviceSize offset=0;
        while(offset<promoted.requirements.size) {
            const VkDeviceSize amount=std::min(chunk,promoted.requirements.size-offset);
            VkDeviceMemory child{}; std::uint32_t usedType=UINT32_MAX; VkResult r=VK_ERROR_OUT_OF_DEVICE_MEMORY;
            for(auto type:types) { VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=amount; ai.memoryTypeIndex=type;
                r=d->allocate(device,&ai,nullptr,&child); if(r==VK_SUCCESS) { usedType=type; break; } }
            if(r!=VK_SUCCESS) { for(auto h:children) d->free(device,h,nullptr); return r; }
            children.push_back(child); childSizes.push_back(amount); childTypes.push_back(usedType);
            VkSparseMemoryBind bind{}; bind.resourceOffset=offset; bind.size=amount; bind.memory=child; binds.push_back(bind);
            offset+=amount;
        }
    } catch(const std::bad_alloc&) { for(auto h:children) d->free(device,h,nullptr); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    VkResult r=bindSparse(device,*d,buffer,binds.data(),static_cast<std::uint32_t>(binds.size()));
    if(r!=VK_SUCCESS) { for(auto h:children) d->free(device,h,nullptr); return r; }
    virtualMem.children=std::move(children); virtualMem.childSizes=std::move(childSizes); virtualMem.childTypes=std::move(childTypes);
    virtualMem.residentBytes=promoted.requirements.size; d->residentBytes+=virtualMem.residentBytes;
    virtualMem.buffer=buffer; virtualMem.bound=true; virtualMem.everBound=true;
    const auto localCount=static_cast<unsigned>(std::count_if(virtualMem.childTypes.begin(),virtualMem.childTypes.end(),[&](auto type){const auto heap=d->memory.memoryTypes[type].heapIndex; return (d->memory.memoryHeaps[heap].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)!=0;}));
    logf("virtual bind bytes=%llu segments=%zu local=%u system=%zu",static_cast<unsigned long long>(promoted.requirements.size),virtualMem.children.size(),localCount,virtualMem.children.size()-localCount);
    promoted.memory=memory; promoted.synthetic=true;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL layerCreateBuffer(VkDevice device,const VkBufferCreateInfo* ci,const VkAllocationCallbacks* allocator,VkBuffer* out) {
    auto d=findDevice(device); if(!d || !d->createBuffer) return VK_ERROR_INITIALIZATION_FAILED;
    if(!eligibleBuffer(*d,ci)) return d->createBuffer(device,ci,allocator,out);
    VkBufferCreateInfo copy=*ci; copy.flags|=VK_BUFFER_CREATE_SPARSE_BINDING_BIT;
    if(d->autoEnabled) copy.usage|=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkResult r=d->createBuffer(device,&copy,allocator,out); if(r!=VK_SUCCESS) return r;
    try { std::lock_guard<std::mutex> lock(d->mutex); d->promotedBuffers.emplace(*out,PromotedBuffer{ci->size}); }
    catch(const std::bad_alloc&) { d->destroyBuffer(device,*out,allocator); *out=VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY; }
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL layerGetBufferMemoryRequirements(VkDevice device,VkBuffer buffer,VkMemoryRequirements* out) {
    auto d=findDevice(device); if(!d || !d->getBufferMemoryRequirements) return;
    d->getBufferMemoryRequirements(device,buffer,out);
    if(!out) return;
    std::lock_guard<std::mutex> lock(d->mutex); auto it=d->promotedBuffers.find(buffer);
    if(it!=d->promotedBuffers.end()) { it->second.requirements=*out; out->memoryTypeBits|=(1u<<d->virtualType); }
}
VKAPI_ATTR void VKAPI_CALL layerGetBufferMemoryRequirements2(VkDevice device,const VkBufferMemoryRequirementsInfo2* info,VkMemoryRequirements2* out) {
    auto d=findDevice(device); if(!d || !d->getBufferMemoryRequirements2 || !info || !out) return;
    d->getBufferMemoryRequirements2(device,info,out);
    std::lock_guard<std::mutex> lock(d->mutex); auto it=d->promotedBuffers.find(info->buffer);
    if(it!=d->promotedBuffers.end()) { it->second.requirements=out->memoryRequirements; out->memoryRequirements.memoryTypeBits|=(1u<<d->virtualType); }
}
VKAPI_ATTR VkResult VKAPI_CALL layerBindBufferMemory(VkDevice device,VkBuffer buffer,VkDeviceMemory memory,VkDeviceSize memoryOffset) {
    auto d=findDevice(device); if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    bool promoted=false,synthetic=false;
    { std::lock_guard<std::mutex> lock(d->mutex); promoted=d->promotedBuffers.count(buffer)!=0; synthetic=d->virtualMemory.count(memory)!=0; }
    if(!promoted) {
        if(synthetic) return VK_ERROR_FEATURE_NOT_PRESENT;
        const VkResult r=d->bindBufferMemory(device,buffer,memory,memoryOffset);
        if(r==VK_SUCCESS) { std::lock_guard<std::mutex> lock(d->mutex); auto i=d->allocations.find(memory); if(i!=d->allocations.end()) i->second.bound=true; }
        return r;
    }
    return bindPromoted(device,d,buffer,memory,memoryOffset);
}
VKAPI_ATTR VkResult VKAPI_CALL layerBindBufferMemory2(VkDevice device,std::uint32_t count,const VkBindBufferMemoryInfo* infos) {
    auto d=findDevice(device); if(!d || !infos) return VK_ERROR_INITIALIZATION_FAILED;
    for(std::uint32_t i=0;i<count;i++) {
        bool promoted=false,synthetic=false; { std::lock_guard<std::mutex> lock(d->mutex); promoted=d->promotedBuffers.count(infos[i].buffer)!=0; synthetic=d->virtualMemory.count(infos[i].memory)!=0; }
        VkResult r;
        if(promoted) {
            if(infos[i].pNext) return VK_ERROR_FEATURE_NOT_PRESENT;
            r=bindPromoted(device,d,infos[i].buffer,infos[i].memory,infos[i].memoryOffset);
        } else if(synthetic) return VK_ERROR_FEATURE_NOT_PRESENT;
        else if(d->bindBufferMemory2) r=d->bindBufferMemory2(device,1,&infos[i]);
        else r=d->bindBufferMemory(device,infos[i].buffer,infos[i].memory,infos[i].memoryOffset);
        if(r==VK_SUCCESS && !promoted) { std::lock_guard<std::mutex> lock(d->mutex); auto ai=d->allocations.find(infos[i].memory); if(ai!=d->allocations.end()) ai->second.bound=true; }
        if(r!=VK_SUCCESS) return r;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL layerMapMemory(VkDevice device,VkDeviceMemory memory,VkDeviceSize offset,VkDeviceSize size,VkMemoryMapFlags flags,void** data) {
    auto d=findDevice(device); if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    { std::lock_guard<std::mutex> lock(d->mutex); if(d->virtualMemory.count(memory)) return VK_ERROR_MEMORY_MAP_FAILED; }
    auto f=reinterpret_cast<PFN_vkMapMemory>(d->gdpa(device,"vkMapMemory")); return f?f(device,memory,offset,size,flags,data):VK_ERROR_FEATURE_NOT_PRESENT;
}
VKAPI_ATTR VkResult VKAPI_CALL layerBindImageMemory(VkDevice device,VkImage image,VkDeviceMemory memory,VkDeviceSize offset) {
    auto d=findDevice(device); if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    { std::lock_guard<std::mutex> lock(d->mutex); if(d->virtualMemory.count(memory)) return VK_ERROR_FEATURE_NOT_PRESENT; }
    auto f=reinterpret_cast<PFN_vkBindImageMemory>(d->gdpa(device,"vkBindImageMemory"));
    const VkResult r=f?f(device,image,memory,offset):VK_ERROR_FEATURE_NOT_PRESENT;
    if(r==VK_SUCCESS) { std::lock_guard<std::mutex> lock(d->mutex); auto i=d->allocations.find(memory); if(i!=d->allocations.end()) i->second.bound=true; }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL layerBindImageMemory2(VkDevice device,std::uint32_t count,const VkBindImageMemoryInfo* infos) {
    auto d=findDevice(device); if(!d || !infos) return VK_ERROR_INITIALIZATION_FAILED;
    { std::lock_guard<std::mutex> lock(d->mutex); for(std::uint32_t i=0;i<count;i++) if(d->virtualMemory.count(infos[i].memory)) return VK_ERROR_FEATURE_NOT_PRESENT; }
    auto f=reinterpret_cast<PFN_vkBindImageMemory2>(d->gdpa(device,"vkBindImageMemory2")); if(!f) f=reinterpret_cast<PFN_vkBindImageMemory2>(d->gdpa(device,"vkBindImageMemory2KHR"));
    const VkResult r=f?f(device,count,infos):VK_ERROR_FEATURE_NOT_PRESENT;
    if(r==VK_SUCCESS) { std::lock_guard<std::mutex> lock(d->mutex); for(std::uint32_t i=0;i<count;i++) { auto a=d->allocations.find(infos[i].memory); if(a!=d->allocations.end()) a->second.bound=true; } }
    return r;
}
VKAPI_ATTR void VKAPI_CALL layerGetDeviceMemoryCommitment(VkDevice device,VkDeviceMemory memory,VkDeviceSize* bytes) {
    auto d=findDevice(device); if(!d || !bytes) return;
    { std::lock_guard<std::mutex> lock(d->mutex); if(d->virtualMemory.count(memory)) { *bytes=0; return; } }
    auto f=reinterpret_cast<PFN_vkGetDeviceMemoryCommitment>(d->gdpa(device,"vkGetDeviceMemoryCommitment")); if(f) f(device,memory,bytes);
}
VKAPI_ATTR VkResult VKAPI_CALL layerGetSnapshotStats(VkDevice device,ZvramSnapshotStatsNX* out) {
    auto d=findDevice(device); if(!d || !out || out->structSize<sizeof(ZvramSnapshotStatsNX)) return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard<std::mutex> lock(d->mutex);
    if(!d->autoEnabled) return VK_ERROR_FEATURE_NOT_PRESENT;
    out->version=1; out->coldLogicalBytes=d->coldLogicalBytes; out->coldStoredBytes=d->coldBytes;
    out->coldBudgetBytes=d->coldBudget; out->residentBytes=d->residentBytes;
    out->freezes=d->freezeCount; out->restores=d->restoreCount; out->failures=d->snapshotFailures;
    out->lastError=static_cast<std::int32_t>(d->lastSnapshotError);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL layerAllocateMemory(VkDevice device,const VkMemoryAllocateInfo* info,const VkAllocationCallbacks* allocator,VkDeviceMemory* out) {
    auto d=findDevice(device); if(!d || !d->allocate) return VK_ERROR_INITIALIZATION_FAILED;
    if(info && info->memoryTypeIndex==d->virtualType) {
        if(!d->virtualEnabled || !out || !priorityOnlyChain(info->pNext)) return VK_ERROR_FEATURE_NOT_PRESENT;
        std::lock_guard<std::mutex> lock(d->mutex);
        if(info->allocationSize==0 || info->allocationSize>d->virtualBytes-d->virtualUsage) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        auto* token=new(std::nothrow) std::uint8_t{}; if(!token) return VK_ERROR_OUT_OF_HOST_MEMORY;
        VkDeviceMemory handle=tokenHandle<VkDeviceMemory>(reinterpret_cast<std::uintptr_t>(token));
        VirtualMemory entry; entry.size=info->allocationSize; entry.token=token;
        try { d->virtualMemory.emplace(handle,std::move(entry)); }
        catch(const std::bad_alloc&) { delete token; return VK_ERROR_OUT_OF_HOST_MEMORY; }
        d->virtualUsage+=info->allocationSize; *out=handle;
        logf("virtual allocate bytes=%llu heap-capacity=%llu",static_cast<unsigned long long>(info->allocationSize),static_cast<unsigned long long>(d->virtualBytes));
        return VK_SUCCESS;
    }
    VkResult r=d->allocate(device,info,allocator,out);
    std::unique_lock<std::mutex> lock(d->mutex);
    if(r!=VK_SUCCESS) { ++d->failures; if(verbose()) logf("allocation failed result=%d",static_cast<int>(r)); return r; }
    bool local=false; if(info->memoryTypeIndex<d->memory.memoryTypeCount) { auto heap=d->memory.memoryTypes[info->memoryTypeIndex].heapIndex; local=(d->memory.memoryHeaps[heap].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)!=0; }
    try { d->allocations.emplace(*out,Allocation{info->allocationSize,local,info->memoryTypeIndex,false}); }
    catch(const std::bad_alloc&) { ++d->failures; lock.unlock(); d->free(device,*out,allocator); *out=VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY; }
    auto& live=local?d->liveLocal:d->liveOther; auto& peak=local?d->peakLocal:d->peakOther; live+=info->allocationSize; peak=std::max(peak,live);
    if(verbose()) logf("allocate %s bytes=%llu",local?"local":"nonlocal",static_cast<unsigned long long>(info->allocationSize));
    return r;
}
VKAPI_ATTR void VKAPI_CALL layerDestroyBuffer(VkDevice device,VkBuffer buffer,const VkAllocationCallbacks* allocator) {
    auto d=findDevice(device); if(!d || !d->destroyBuffer) return;
    std::lock_guard<std::mutex> lock(d->mutex);
    auto it=d->promotedBuffers.find(buffer);
    VkDeviceMemory memory=VK_NULL_HANDLE;
    auto vi=d->virtualMemory.end();
    bool nativeAllocation=false;
    if(it!=d->promotedBuffers.end() && it->second.memory && it->second.requirements.size) {
        memory=it->second.memory;
        vi=d->virtualMemory.find(memory);
        if(vi!=d->virtualMemory.end()) {
            auto& vm=vi->second;
            if(vm.bound && vm.children.empty()) vm.bound=false;
            if(vm.bound) {
                std::lock_guard<std::mutex> queueLock(d->queueMutex);
                std::vector<VkSparseMemoryBind> unbinds;
                bool canUnbind=true;
                try { unbinds.resize(vm.children.size()); }
                catch(const std::bad_alloc&) { canUnbind=false; logf("sparse unbind before buffer destroy skipped: host memory exhausted; retaining backing"); }
                if(canUnbind) {
                    VkDeviceSize at=0; for(std::size_t i=0;i<unbinds.size();i++) { unbinds[i].resourceOffset=at; unbinds[i].size=vm.childSizes[i]; at+=vm.childSizes[i]; }
                    const auto unbindResult=bindSparseLocked(device,*d,buffer,unbinds.data(),static_cast<std::uint32_t>(unbinds.size()));
                    if(unbindResult==VK_SUCCESS) vm.bound=false;
                    else logf("sparse unbind before buffer destroy failed result=%d; retaining backing",static_cast<int>(unbindResult));
                }
            }
        } else {
            nativeAllocation=true;
            std::lock_guard<std::mutex> queueLock(d->queueMutex);
            VkSparseMemoryBind unbind{}; unbind.size=it->second.requirements.size;
            const auto unbindResult=bindSparseLocked(device,*d,buffer,&unbind,1);
            if(unbindResult!=VK_SUCCESS) logf("native sparse unbind before buffer destroy failed result=%d",static_cast<int>(unbindResult));
        }
    }
    d->destroyBuffer(device,buffer,allocator);
    if(vi!=d->virtualMemory.end()) {
        auto& vm=vi->second;
        vm.bound=false; vm.buffer=VK_NULL_HANDLE;
        if(vm.deferredFree) {
            if(vm.residentBytes) d->residentBytes-=vm.residentBytes;
            d->coldBytes-=vm.coldStoredBytes; d->coldLogicalBytes-=vm.coldLogicalSize;
            releaseChildren(*d,vm); d->virtualUsage-=vm.size;
            delete static_cast<std::uint8_t*>(vm.token); d->virtualMemory.erase(vi);
        }
    } else if(nativeAllocation) {
        auto ai=d->allocations.find(memory); if(ai!=d->allocations.end()) ai->second.bound=false;
    }
    d->promotedBuffers.erase(buffer);
}
VKAPI_ATTR void VKAPI_CALL layerDestroyDevice(VkDevice device,const VkAllocationCallbacks* allocator) {
    auto d=findDevice(device); if(!d) return;
    d->stopWorker.store(true); d->activity.notify_all();
    if(d->snapshotWorker.joinable()) d->snapshotWorker.join();
    { std::lock_guard<std::mutex> lock(d->mutex);
      for(auto& pair:d->virtualMemory) { releaseChildren(*d,pair.second); delete static_cast<std::uint8_t*>(pair.second.token); }
      d->virtualMemory.clear();
      releaseSnapshotResources(*d);
      logf("device=%s live-local=%llu peak-local=%llu live-nonlocal=%llu peak-nonlocal=%llu allocation-failures=%llu",d->gpu.c_str(),static_cast<unsigned long long>(d->liveLocal),static_cast<unsigned long long>(d->peakLocal),static_cast<unsigned long long>(d->liveOther),static_cast<unsigned long long>(d->peakOther),static_cast<unsigned long long>(d->failures)); }
    { std::lock_guard<std::mutex> lock(mapsMutex); devices.erase(key(device)); }
    if(d->destroy) d->destroy(device,allocator);
}
VKAPI_ATTR void VKAPI_CALL layerFreeMemory(VkDevice device,VkDeviceMemory memory,const VkAllocationCallbacks* allocator) {
    auto d=findDevice(device); if(!d || !d->free) return;
    { std::lock_guard<std::mutex> lock(d->mutex); auto i=d->virtualMemory.find(memory);
      if(i!=d->virtualMemory.end()) {
          if(i->second.bound) { i->second.deferredFree=true; return; }
          if(i->second.residentBytes) d->residentBytes-=i->second.residentBytes;
          d->coldBytes-=i->second.coldStoredBytes; d->coldLogicalBytes-=i->second.coldLogicalSize;
          releaseChildren(*d,i->second); d->virtualUsage-=i->second.size;
          logf("virtual free bytes=%llu",static_cast<unsigned long long>(i->second.size));
          delete static_cast<std::uint8_t*>(i->second.token); d->virtualMemory.erase(i); return;
      } }
    Allocation a{}; bool found=false;
    { std::lock_guard<std::mutex> lock(d->mutex); auto i=d->allocations.find(memory);
      if(i!=d->allocations.end()) { a=i->second; auto& live=a.local?d->liveLocal:d->liveOther; live-=a.size; d->allocations.erase(i); found=true; } }
    if(found && verbose()) logf("free bytes=%llu",static_cast<unsigned long long>(a.size));
    d->free(device,memory,allocator);
}

template<class Function,class... Args>
VkResult queueCall(VkQueue queue,const char* name,Args... args) {
    auto d=findDevice(reinterpret_cast<VkDevice>(queue)); if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    auto next=reinterpret_cast<Function>(d->gdpa(d->handle,name)); if(!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    std::unique_lock<std::mutex> deviceLock(d->mutex,std::defer_lock);
    std::unique_lock<std::mutex> queueLock(d->queueMutex,std::defer_lock);
    if(d->virtualEnabled) { deviceLock.lock(); queueLock.lock(); }
    if(d->autoEnabled) { const auto r=restoreColdLocked(d->handle,*d); if(r!=VK_SUCCESS) return r; }
    const auto r=next(queue,args...);
    if(d->autoEnabled) { d->lastActivity=std::chrono::steady_clock::now(); d->activity.notify_all(); }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL layerQueueSubmit(VkQueue q,std::uint32_t count,const VkSubmitInfo* info,VkFence fence) {
    return queueCall<PFN_vkQueueSubmit>(q,"vkQueueSubmit",count,info,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL layerQueueSubmit2(VkQueue q,std::uint32_t count,const VkSubmitInfo2* info,VkFence fence) {
    return queueCall<PFN_vkQueueSubmit2>(q,"vkQueueSubmit2",count,info,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL layerQueueSubmit2KHR(VkQueue q,std::uint32_t count,const VkSubmitInfo2* info,VkFence fence) {
    return queueCall<PFN_vkQueueSubmit2>(q,"vkQueueSubmit2KHR",count,info,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL layerQueueBindSparse(VkQueue q,std::uint32_t count,const VkBindSparseInfo* info,VkFence fence) {
    return queueCall<PFN_vkQueueBindSparse>(q,"vkQueueBindSparse",count,info,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL layerQueueWaitIdle(VkQueue q) {
    return queueCall<PFN_vkQueueWaitIdle>(q,"vkQueueWaitIdle");
}
VKAPI_ATTR VkResult VKAPI_CALL layerQueuePresent(VkQueue q,const VkPresentInfoKHR* info) {
    return queueCall<PFN_vkQueuePresentKHR>(q,"vkQueuePresentKHR",info);
}
VKAPI_ATTR VkResult VKAPI_CALL layerDeviceWaitIdle(VkDevice device) {
    auto d=findDevice(device); if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    auto next=reinterpret_cast<PFN_vkDeviceWaitIdle>(d->gdpa(device,"vkDeviceWaitIdle")); if(!next) return VK_ERROR_INITIALIZATION_FAILED;
    std::unique_lock<std::mutex> deviceLock(d->mutex,std::defer_lock);
    std::unique_lock<std::mutex> queueLock(d->queueMutex,std::defer_lock);
    if(d->virtualEnabled) { deviceLock.lock(); queueLock.lock(); }
    if(d->autoEnabled) { const auto r=restoreColdLocked(device,*d); if(r!=VK_SUCCESS) return r; }
    const auto r=next(device);
    if(d->autoEnabled) { d->lastActivity=std::chrono::steady_clock::now(); d->activity.notify_all(); }
    return r;
}

PFN_vkVoidFunction lookup(const char* name) {
    if(!name) return nullptr;
#define MATCH(n,f) if(std::strcmp(name,n)==0) return reinterpret_cast<PFN_vkVoidFunction>(f)
    MATCH("vkGetInstanceProcAddr",layerGetInstanceProcAddr); MATCH("vkGetDeviceProcAddr",layerGetDeviceProcAddr);
    MATCH("vk_layerGetPhysicalDeviceProcAddr",layerGetPhysicalDeviceProcAddr);
    MATCH("vkCreateInstance",layerCreateInstance); MATCH("vkDestroyInstance",layerDestroyInstance);
    MATCH("vkCreateDevice",layerCreateDevice); MATCH("vkDestroyDevice",layerDestroyDevice);
    MATCH("vkGetPhysicalDeviceMemoryProperties",layerGetPhysicalDeviceMemoryProperties);
    MATCH("vkGetPhysicalDeviceMemoryProperties2",layerGetPhysicalDeviceMemoryProperties2);
    MATCH("vkGetPhysicalDeviceMemoryProperties2KHR",layerGetPhysicalDeviceMemoryProperties2);
    MATCH("vkAllocateMemory",layerAllocateMemory); MATCH("vkFreeMemory",layerFreeMemory);
    MATCH("vkCreateBuffer",layerCreateBuffer); MATCH("vkDestroyBuffer",layerDestroyBuffer);
    MATCH("vkGetBufferMemoryRequirements",layerGetBufferMemoryRequirements);
    MATCH("vkGetBufferMemoryRequirements2",layerGetBufferMemoryRequirements2); MATCH("vkGetBufferMemoryRequirements2KHR",layerGetBufferMemoryRequirements2);
    MATCH("vkBindBufferMemory",layerBindBufferMemory);
    MATCH("vkBindBufferMemory2",layerBindBufferMemory2); MATCH("vkBindBufferMemory2KHR",layerBindBufferMemory2);
    MATCH("vkMapMemory",layerMapMemory); MATCH("vkBindImageMemory",layerBindImageMemory);
    MATCH("vkBindImageMemory2",layerBindImageMemory2); MATCH("vkBindImageMemory2KHR",layerBindImageMemory2);
    MATCH("vkGetDeviceMemoryCommitment",layerGetDeviceMemoryCommitment);
    MATCH("vkZVramGetSnapshotStatsNX",layerGetSnapshotStats);
    MATCH("vkQueueSubmit",layerQueueSubmit); MATCH("vkQueueSubmit2",layerQueueSubmit2);
    MATCH("vkQueueSubmit2KHR",layerQueueSubmit2KHR); MATCH("vkQueueBindSparse",layerQueueBindSparse);
    MATCH("vkQueueWaitIdle",layerQueueWaitIdle); MATCH("vkQueuePresentKHR",layerQueuePresent);
    MATCH("vkDeviceWaitIdle",layerDeviceWaitIdle);
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
    auto d=findDevice(device);
    if(name && (std::strcmp(name,"vkQueuePresentKHR")==0 || std::strcmp(name,"vkQueueSubmit2")==0 || std::strcmp(name,"vkQueueSubmit2KHR")==0))
        if(!d || !d->gdpa || !d->gdpa(device,name)) return nullptr;
    if(auto f=lookup(name)) return f;
    return d&&d->gdpa?d->gdpa(device,name):nullptr;
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
