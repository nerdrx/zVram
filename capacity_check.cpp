#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint64_t MiB = 1024ull * 1024ull;
constexpr uint64_t MaxMiB = 40960;

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed: " + std::to_string(r));
}

struct Buffer {
    VkDevice device{};
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    Buffer() = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& o) noexcept : device(o.device), buffer(o.buffer), memory(o.memory) {
        o.buffer = VK_NULL_HANDLE;
        o.memory = VK_NULL_HANDLE;
    }
    Buffer& operator=(Buffer&& o) noexcept {
        if (this != &o) { reset(); device=o.device; buffer=o.buffer; memory=o.memory; o.buffer={}; o.memory={}; }
        return *this;
    }
    ~Buffer() { reset(); }
    void reset() {
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
        buffer = VK_NULL_HANDLE; memory = VK_NULL_HANDLE;
    }
};

uint64_t parseMiB(const char* s, const char* name, uint64_t max) {
    if (!s || !*s) throw std::runtime_error(std::string("invalid ") + name);
    uint64_t n = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || n > (max - static_cast<unsigned>(*s - '0')) / 10)
            throw std::runtime_error(std::string("invalid ") + name + " (expected 1.." + std::to_string(max) + ")");
        n = n * 10 + static_cast<unsigned>(*s - '0');
    }
    if (!n || n > max) throw std::runtime_error(std::string("invalid ") + name + " (expected 1.." + std::to_string(max) + ")");
    return n;
}

uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

bool readNumber(const std::string& path, uint64_t& n) {
    std::ifstream f(path);
    return bool(f >> n);
}

uint64_t memAvailableBytes() {
    std::ifstream f("/proc/meminfo");
    std::string key, unit;
    uint64_t kib;
    while (f >> key >> kib >> unit) if (key == "MemAvailable:") return kib * 1024;
    return 0;
}

void sysfsSnapshot(const VkPhysicalDeviceDrmPropertiesEXT& drm, const char* label) {
    if (!drm.hasPrimary || drm.primaryMajor == 0) {
        std::cout << "sysfs " << label << ": unavailable (Vulkan DRM identity missing)\n";
        return;
    }
    // Resolve the DRM primary node through its sysfs device link; this avoids card-index assumptions.
    const std::string link = "/sys/dev/char/" + std::to_string(drm.primaryMajor) + ":" + std::to_string(drm.primaryMinor) + "/device";
    char resolved[4096];
    if (!realpath(link.c_str(), resolved)) {
        std::cout << "sysfs " << label << ": unavailable (cannot resolve DRM device)\n";
        return;
    }
    const std::string base(resolved);
    uint64_t vt=0, vu=0, gt=0, gu=0;
    bool v = readNumber(base + "/mem_info_vram_total", vt) && readNumber(base + "/mem_info_vram_used", vu);
    bool g = readNumber(base + "/mem_info_gtt_total", gt) && readNumber(base + "/mem_info_gtt_used", gu);
    std::cout << "sysfs " << label << ": ";
    if (v) std::cout << "VRAM=" << vu << "/" << vt << " B"; else std::cout << "VRAM unavailable";
    if (g) std::cout << " GTT=" << gu << "/" << gt << " B"; else std::cout << " GTT unavailable";
    std::cout << "\n";
}
} // namespace

int main(int argc, char** argv) try {
    uint64_t totalMiB = 64, chunkMiB = 64;
    for (int i=1; i<argc; ++i) {
        const std::string arg(argv[i]);
        if ((arg == "--mib" || arg == "--chunk-mib") && i+1 < argc) {
            const uint64_t n = parseMiB(argv[++i], arg.c_str(), arg == "--mib" ? MaxMiB : 256);
            if (arg == "--mib") totalMiB=n; else chunkMiB=n;
        } else throw std::runtime_error("usage: zvram-capacity-check [--mib 1..40960] [--chunk-mib 1..256]");
    }
    const uint64_t total = totalMiB * MiB, chunk = chunkMiB * MiB;
    const uint64_t hostAvail = memAvailableBytes(), hostBudget = hostAvail > 8*1024ull*MiB ? hostAvail-8*1024ull*MiB : 0;

    VkInstance instance{};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app;
    check(vkCreateInstance(&ici,nullptr,&instance), "vkCreateInstance");
    VkDevice device{}; VkCommandPool pool{}; VkQueue queue{};
    auto cleanup = [&] { if(device) vkDeviceWaitIdle(device); if(pool) vkDestroyCommandPool(device,pool,nullptr); if(device) vkDestroyDevice(device,nullptr); if(instance) vkDestroyInstance(instance,nullptr); };
    try {
        uint32_t count=0; check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"enumerate devices");
        if (!count) throw std::runtime_error("no Vulkan physical device");
        std::vector<VkPhysicalDevice> physicals(count);
        check(vkEnumeratePhysicalDevices(instance,&count,physicals.data()),"enumerate devices");
        VkPhysicalDevice gpu{}; uint32_t family=UINT32_MAX;
        for (auto p : physicals) {
            VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(p,&props);
            if (props.vendorID != 0x1002 || props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
            uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(p,&nq,nullptr);
            std::vector<VkQueueFamilyProperties> qs(nq); vkGetPhysicalDeviceQueueFamilyProperties(p,&nq,qs.data());
            for(uint32_t i=0;i<nq;++i) if(qs[i].queueCount && (qs[i].queueFlags&VK_QUEUE_TRANSFER_BIT)){gpu=p; family=i; break;}
            if(gpu) break;
        }
        if(!gpu) throw std::runtime_error("no discrete AMD Vulkan GPU with a transfer queue");
        VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(gpu,&props);
        uint32_t ne=0; check(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&ne,nullptr),"enumerate extensions");
        std::vector<VkExtensionProperties> exts(ne); check(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&ne,exts.data()),"enumerate extensions");
        auto hasExt=[&](const char* name){return std::any_of(exts.begin(),exts.end(),[&](const auto& e){return std::strcmp(e.extensionName,name)==0;});};
        VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; if(hasExt(VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)) props2.pNext=&drm;
        vkGetPhysicalDeviceProperties2(gpu,&props2);

        const bool hasBudget=hasExt(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        const bool hasPriority=hasExt(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
        VkPhysicalDeviceMemoryPriorityFeaturesEXT priorityFeature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        if(hasPriority) features.pNext=&priorityFeature;
        vkGetPhysicalDeviceFeatures2(gpu,&features);
        const bool usePriority=hasPriority && priorityFeature.memoryPriority;
        std::vector<const char*> enabled;
        if(hasBudget) enabled.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        if(usePriority) enabled.push_back(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
        float qp=1.0f; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex=family; qci.queueCount=1; qci.pQueuePriorities=&qp;
        VkPhysicalDeviceMemoryPriorityFeaturesEXT enablePriority{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT}; enablePriority.memoryPriority=usePriority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
        dci.enabledExtensionCount=static_cast<uint32_t>(enabled.size()); dci.ppEnabledExtensionNames=enabled.data();
        if(usePriority) dci.pNext=&enablePriority;
        check(vkCreateDevice(gpu,&dci,nullptr,&device),"vkCreateDevice"); vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex=family; pci.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        check(vkCreateCommandPool(device,&pci,nullptr,&pool),"vkCreateCommandPool");

        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 mp2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2}; if(hasBudget) mp2.pNext=&budget;
        vkGetPhysicalDeviceMemoryProperties2(gpu,&mp2);
        uint32_t localHeap=UINT32_MAX; uint64_t localBytes=0, freeVram=0;
        for(uint32_t i=0;i<mp2.memoryProperties.memoryHeapCount;++i) {
            auto& h=mp2.memoryProperties.memoryHeaps[i];
            if((h.flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && h.size>localBytes) {
                localHeap=i; localBytes=h.size;
                freeVram=hasBudget && budget.heapBudget[i]>budget.heapUsage[i] ? budget.heapBudget[i]-budget.heapUsage[i] : 0;
            }
        }
        if(localHeap==UINT32_MAX) throw std::runtime_error("AMD Vulkan device has no device-local heap");
        sysfsSnapshot(drm,"baseline");
        uint64_t gttTotal=0,gttUsed=0;
        bool hasGtt=false;
        if(drm.hasPrimary && drm.primaryMajor) {
            const std::string link="/sys/dev/char/"+std::to_string(drm.primaryMajor)+":"+std::to_string(drm.primaryMinor)+"/device";
            char resolved[4096];
            if(realpath(link.c_str(),resolved)) hasGtt=readNumber(std::string(resolved)+"/mem_info_gtt_total",gttTotal)&&readNumber(std::string(resolved)+"/mem_info_gtt_used",gttUsed);
        }
        if(!hasBudget || !freeVram) throw std::runtime_error("cannot estimate conservative free VRAM (VK_EXT_memory_budget unavailable or reports none)");
        if(!hasGtt) throw std::runtime_error("cannot estimate AMD GTT capacity from matched DRM sysfs device");
        const uint64_t gttFree=gttTotal>gttUsed?gttTotal-gttUsed:0;
        const uint64_t capacity=freeVram+std::min(gttFree,hostBudget);
        std::cout<<"GPU: "<<props.deviceName<<"\nlocal heap="<<localBytes<<" B; VRAM free="<<freeVram<<" B; GTT free="<<gttFree<<" B; host MemAvailable="<<hostAvail<<" B; conservative capacity estimate="<<capacity<<" B\n";
        std::cout<<"requested="<<total<<" B in "<<((total+chunk-1)/chunk)<<" chunks; staging="<<std::min(total,chunk)<<" B\n";
        if(total>capacity) throw std::runtime_error("request exceeds conservative capacity estimate; refusing allocation");

        VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(gpu,&mp);
        auto make=[&](VkDeviceSize size,VkBufferUsageFlags usage,VkMemoryPropertyFlags flags,bool lowPriority) {
            Buffer b; b.device=device; VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size=size; ci.usage=usage; ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            check(vkCreateBuffer(device,&ci,nullptr,&b.buffer),"vkCreateBuffer"); VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device,b.buffer,&req);
            uint32_t type=UINT32_MAX;
            for(uint32_t i=0;i<mp.memoryTypeCount;++i) if((req.memoryTypeBits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&flags)==flags &&
                (!(flags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || !(mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))){if(type==UINT32_MAX) type=i; if(!(flags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || (mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {type=i;break;}}
            if(type==UINT32_MAX) throw std::runtime_error("no compatible Vulkan memory type");
            VkMemoryPriorityAllocateInfoEXT pi{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT}; pi.priority=lowPriority?0.0f:1.0f;
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=type; if(usePriority) ai.pNext=&pi;
            try { check(vkAllocateMemory(device,&ai,nullptr,&b.memory),"vkAllocateMemory"); check(vkBindBufferMemory(device,b.buffer,b.memory,0),"vkBindBufferMemory"); }
            catch(...) { b.reset(); throw; }
            return b;
        };
        auto copy=[&](VkBuffer src,VkBuffer dst,VkDeviceSize size) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool=pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1;
            VkCommandBuffer cmd{}; check(vkAllocateCommandBuffers(device,&ai,&cmd),"allocate command buffer");
            try {
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                check(vkBeginCommandBuffer(cmd,&bi),"begin command buffer");
                VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
                VkBufferCopy region{0,0,size}; vkCmdCopyBuffer(cmd,src,dst,1,&region);
                barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
                vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
                check(vkEndCommandBuffer(cmd),"end command buffer"); VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&cmd;
                check(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE),"queue submit"); check(vkQueueWaitIdle(queue),"queue wait");
            } catch(...) { vkFreeCommandBuffers(device,pool,1,&cmd); throw; }
            vkFreeCommandBuffers(device,pool,1,&cmd);
        };

        const uint64_t stageBytes=std::min(total,chunk);
        Buffer staging=make(stageBytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,false);
        void* mapped=nullptr; check(vkMapMemory(device,staging.memory,0,stageBytes,0,&mapped),"map staging");
        std::vector<Buffer> resident; resident.reserve(static_cast<size_t>((total+chunk-1)/chunk));
        const auto start=Clock::now();
        for(uint64_t offset=0;offset<total;) {
            const uint64_t bytes=std::min(chunk,total-offset);
            resident.push_back(make(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,true));
            auto* words=static_cast<uint64_t*>(mapped);
            const uint64_t seed=0x7a5652414dull ^ (offset/chunk);
            for(uint64_t i=0;i<bytes/8;++i) words[i]=splitmix64((offset/8+i)^seed);
            copy(staging.buffer,resident.back().buffer,bytes);
            offset+=bytes;
            if(offset==total || offset%(1024*MiB)<bytes) std::cout<<"uploaded "<<offset<<" / "<<total<<" B\n";
        }
        // Host-visible staging may be uncached. Copy once into cached RAM before checking words.
        std::vector<uint64_t> readback(stageBytes/8);
        sysfsSnapshot(drm,"all chunks uploaded");
        uint64_t verified=0, chunkIndex=0;
        for(auto& b:resident) {
            const uint64_t offset=chunkIndex*chunk;
            const uint64_t bytes=std::min(chunk,total-offset);
            copy(b.buffer,staging.buffer,bytes);
            std::memcpy(readback.data(),mapped,bytes);
            const auto* words=readback.data();
            const uint64_t seed=0x7a5652414dull ^ chunkIndex;
            for(uint64_t i=0;i<bytes/8;++i) if(words[i]!=splitmix64((offset/8+i)^seed))
                throw std::runtime_error("readback mismatch at byte " + std::to_string(offset+i*8));
            verified+=bytes; ++chunkIndex;
            if(verified==total || verified%(1024*MiB)<bytes) std::cout<<"verified "<<verified<<" / "<<total<<" B\n";
        }
        const double seconds=std::chrono::duration<double>(Clock::now()-start).count();
        std::cout<<"elapsed generation+verification="<<seconds<<" s\n";
        sysfsSnapshot(drm,"after readback");
        if(verified!=total) throw std::runtime_error("verification byte count mismatch");
        vkUnmapMemory(device,staging.memory); staging.reset(); resident.clear();
        sysfsSnapshot(drm,"after cleanup");
        std::cout<<"PASS: verified "<<verified<<" bytes across "<<chunkIndex<<" resident device-local allocations\n";
    } catch(...) { cleanup(); throw; }
    cleanup(); return 0;
} catch(const std::exception& e) { std::cerr<<"error: "<<e.what()<<"\n"; return 1; }
