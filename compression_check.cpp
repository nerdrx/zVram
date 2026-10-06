#include <vulkan/vulkan.h>
#include <zstd.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
static void vkcheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed: " + std::to_string(r));
}
struct Buffer {
    VkDevice device{}; VkBuffer buffer{}; VkDeviceMemory memory{}; VkDeviceSize size{};
    Buffer() = default; Buffer(const Buffer&) = delete; Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& o) noexcept : device(o.device), buffer(o.buffer), memory(o.memory), size(o.size) { o.buffer={}; o.memory={}; }
    ~Buffer() { reset(); }
    void reset() { if(buffer) vkDestroyBuffer(device,buffer,nullptr); if(memory) vkFreeMemory(device,memory,nullptr); buffer={}; memory={}; }
};

int main() try {
    constexpr size_t bytes = 16u * 1024u * 1024u;
    VkInstance instance{};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app;
    vkcheck(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
    VkDevice device{}; VkCommandPool pool{}; VkQueue queue{};
    auto cleanup = [&] { if (device) vkDeviceWaitIdle(device); if (pool) vkDestroyCommandPool(device, pool, nullptr); if (device) vkDestroyDevice(device, nullptr); vkDestroyInstance(instance, nullptr); };
    try {
        uint32_t count = 0; vkcheck(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate devices");
        if (!count) throw std::runtime_error("no Vulkan physical device");
        std::vector<VkPhysicalDevice> devices(count); vkcheck(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate devices");
        VkPhysicalDevice gpu{}; uint32_t family = UINT32_MAX;
        for (auto d : devices) {
            VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(d, &p);
            if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU || p.vendorID != 0x1002) continue;
            uint32_t n = 0; vkGetPhysicalDeviceQueueFamilyProperties(d, &n, nullptr);
            std::vector<VkQueueFamilyProperties> q(n); vkGetPhysicalDeviceQueueFamilyProperties(d, &n, q.data());
            for (uint32_t i = 0; i < n; ++i) if (q[i].queueCount && (q[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) { gpu=d; family=i; break; }
            if (gpu) break;
        }
        if (!gpu) throw std::runtime_error("no discrete AMD Vulkan GPU with a transfer queue");
        VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(gpu, &props);
        uint32_t extCount=0; vkEnumerateDeviceExtensionProperties(gpu,nullptr,&extCount,nullptr);
        std::vector<VkExtensionProperties> exts(extCount); vkEnumerateDeviceExtensionProperties(gpu,nullptr,&extCount,exts.data());
        bool hasBudget=std::any_of(exts.begin(),exts.end(),[](const auto& e){return std::strcmp(e.extensionName,VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)==0;});
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 mem2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2}; if(hasBudget) mem2.pNext=&budget;
        vkGetPhysicalDeviceMemoryProperties2(gpu,&mem2);
        float priority=1.0f; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex=family; qci.queueCount=1; qci.pQueuePriorities=&priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
        vkcheck(vkCreateDevice(gpu, &dci, nullptr, &device), "vkCreateDevice"); vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex=family; pci.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        vkcheck(vkCreateCommandPool(device,&pci,nullptr,&pool),"vkCreateCommandPool");
        VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(gpu,&mp);
        auto make = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags) {
            Buffer b{}; b.device=device; b.size=size; VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size=size; ci.usage=usage; ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            vkcheck(vkCreateBuffer(device,&ci,nullptr,&b.buffer),"vkCreateBuffer"); VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device,b.buffer,&req);
            uint32_t type=UINT32_MAX; for(uint32_t i=0;i<mp.memoryTypeCount;i++) if((req.memoryTypeBits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&flags)==flags && (!(flags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || !(mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))){if(type==UINT32_MAX) type=i; if(!(flags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || (mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {type=i;break;}}
            if(type==UINT32_MAX) throw std::runtime_error("no compatible Vulkan memory type");
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=type;
            try { vkcheck(vkAllocateMemory(device,&ai,nullptr,&b.memory),"vkAllocateMemory"); vkcheck(vkBindBufferMemory(device,b.buffer,b.memory,0),"vkBindBufferMemory"); }
            catch(...) { b.reset(); throw; }
            b.size=req.size; return b;
        };
        auto copy = [&](VkBuffer src, VkBuffer dst) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool=pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1;
            VkCommandBuffer cmd{}; vkcheck(vkAllocateCommandBuffers(device,&ai,&cmd),"allocate command buffer");
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            try { vkcheck(vkBeginCommandBuffer(cmd,&bi),"begin command buffer");
                VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
                VkBufferCopy region{0,0,bytes}; vkCmdCopyBuffer(cmd,src,dst,1,&region);
                barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
                vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
                vkcheck(vkEndCommandBuffer(cmd),"end command buffer");
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&cmd; vkcheck(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE),"queue submit"); vkcheck(vkQueueWaitIdle(queue),"queue wait"); }
            catch(...) { vkFreeCommandBuffers(device,pool,1,&cmd); throw; }
            vkFreeCommandBuffers(device,pool,1,&cmd);
        };
        auto staging = make(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        void* mapped=nullptr; vkcheck(vkMapMemory(device,staging.memory,0,VK_WHOLE_SIZE,0,&mapped),"map staging");
        std::vector<uint8_t> check(bytes), payload(bytes); std::mt19937 rng(0x7900);
        auto run = [&](const char* name, bool random) {
            if(random) for(auto& x:payload) x=static_cast<uint8_t>(rng()); else for(size_t i=0;i<bytes;i++) payload[i]=static_cast<uint8_t>((i%4096<3072)?(i%17):((i/4096)%256));
            std::memcpy(mapped,payload.data(),bytes);
            Buffer gpuBuf=make(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            auto t0=Clock::now(); copy(staging.buffer,gpuBuf.buffer); copy(gpuBuf.buffer,staging.buffer); auto t1=Clock::now();
            std::memcpy(check.data(),mapped,bytes); if(check!=payload) throw std::runtime_error("initial copy-back mismatch");
            size_t bound=ZSTD_compressBound(bytes); std::vector<uint8_t> compressed(bound);
            auto c0=Clock::now(); size_t n=ZSTD_compress(compressed.data(),bound,check.data(),bytes,3); auto c1=Clock::now();
            if(ZSTD_isError(n)) throw std::runtime_error(ZSTD_getErrorName(n));
            bool raw=n>=bytes;
            if(raw) { std::vector<uint8_t> exact(check); compressed.swap(exact); } else { std::vector<uint8_t> exact(compressed.begin(),compressed.begin()+n); compressed.swap(exact); }
            VkDeviceSize freed=gpuBuf.size; gpuBuf.reset();
            std::vector<uint8_t> restored(bytes); auto d0=Clock::now();
            if(raw) restored=compressed; else { size_t out=ZSTD_decompress(restored.data(),bytes,compressed.data(),compressed.size()); if(ZSTD_isError(out)||out!=bytes) throw std::runtime_error("zstd decompression failed"); }
            auto d1=Clock::now(); if(restored!=payload) throw std::runtime_error("decompressed bytes mismatch");
            std::memcpy(mapped,restored.data(),bytes); Buffer again=make(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            auto u0=Clock::now(); copy(staging.buffer,again.buffer); copy(again.buffer,staging.buffer); auto u1=Clock::now();
            std::memcpy(check.data(),mapped,bytes); if(check!=payload) throw std::runtime_error("reallocated GPU copy-back mismatch"); again.reset();
            auto ms=[](auto a,auto b){return std::chrono::duration<double,std::milli>(b-a).count();};
            std::cout<<name<<": original="<<bytes<<" stored="<<compressed.size()<<" B mode="<<(raw?"raw":"zstd")<<" ratio="<<double(compressed.size())/bytes<<" released-device-allocation="<<freed<<" B, initial-copy="<<ms(t0,t1)<<" ms compress="<<ms(c0,c1)<<" ms decompress="<<ms(d0,d1)<<" ms restore-copy="<<ms(u0,u1)<<" ms\n";
        };
        std::cout<<"GPU: "<<props.deviceName<<"\n";
        for(uint32_t i=0;i<mem2.memoryProperties.memoryHeapCount;i++) { const auto& h=mem2.memoryProperties.memoryHeaps[i]; std::cout<<"heap["<<i<<"] size="<<h.size<<" B"; if(hasBudget) std::cout<<" budget="<<budget.heapBudget[i]<<" B usage="<<budget.heapUsage[i]<<" B"; if(h.flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) std::cout<<" device-local"; std::cout<<"\n"; }
        run("repetitive",false); run("seeded-random",true);
        vkUnmapMemory(device,staging.memory); staging.reset();
    } catch(...) { cleanup(); throw; }
    cleanup(); return 0;
} catch(const std::exception& e) { std::cerr<<"error: "<<e.what()<<"\n"; return 1; }
