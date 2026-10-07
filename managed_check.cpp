#include "managed_pool.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr std::size_t MiB = 1024u * 1024u;
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::uint32_t mix32(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; return x ^ (x >> 16);
}
template<class F> void mustFail(F&& operation, const char* message) {
    bool failed = false;
    try { operation(); } catch (const std::exception&) { failed = true; }
    require(failed, message);
}

struct Context {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    std::uint32_t family{};
    VkCommandPool commands{};
    VkDescriptorSetLayout descriptorLayout{};
    VkDescriptorPool descriptors{};
    VkDescriptorSet descriptor{};
    VkPipelineLayout pipelineLayout{};
    VkPipeline pipeline{};
    VkPhysicalDeviceMemoryProperties memory{};

    ~Context() {
        if (device) vkDeviceWaitIdle(device);
        if (pipeline) vkDestroyPipeline(device,pipeline,nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device,pipelineLayout,nullptr);
        if (descriptors) vkDestroyDescriptorPool(device,descriptors,nullptr);
        if (descriptorLayout) vkDestroyDescriptorSetLayout(device,descriptorLayout,nullptr);
        if (commands) vkDestroyCommandPool(device,commands,nullptr);
        if (device) vkDestroyDevice(device,nullptr);
        if (instance) vkDestroyInstance(instance,nullptr);
    }
    void initialize(const char* shaderPath) {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app;
        check(vkCreateInstance(&ici,nullptr,&instance),"create instance");
        std::uint32_t count=0; check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"enumerate GPUs");
        std::vector<VkPhysicalDevice> GPUs(count); check(vkEnumeratePhysicalDevices(instance,&count,GPUs.data()),"enumerate GPUs");
        for (auto candidate:GPUs) {
            VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(candidate,&properties);
            if(properties.vendorID!=0x1002 || properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
            std::uint32_t n=0; vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,nullptr);
            std::vector<VkQueueFamilyProperties> queues(n); vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,queues.data());
            for(std::uint32_t i=0;i<n;++i) if(queues[i].queueCount && (queues[i].queueFlags&VK_QUEUE_COMPUTE_BIT)) {
                physical=candidate; family=i; break;
            }
            if(physical) { std::cout<<"GPU: "<<properties.deviceName<<'\n'; break; }
        }
        require(physical!=VK_NULL_HANDLE,"no discrete AMD GPU with compute queue");
        vkGetPhysicalDeviceMemoryProperties(physical,&memory);
        float priority=1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex=family; qci.queueCount=1; qci.pQueuePriorities=&priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
        check(vkCreateDevice(physical,&dci,nullptr,&device),"create device"); vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cp.queueFamilyIndex=family; cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device,&cp,nullptr,&commands),"create command pool");
        VkDescriptorSetLayoutBinding binding{}; binding.binding=0; binding.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; binding.descriptorCount=1; binding.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount=1; dl.pBindings=&binding;
        check(vkCreateDescriptorSetLayout(device,&dl,nullptr,&descriptorLayout),"create descriptor layout");
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=1; dp.poolSizeCount=1; dp.pPoolSizes=&ps;
        check(vkCreateDescriptorPool(device,&dp,nullptr,&descriptors),"create descriptor pool");
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool=descriptors; da.descriptorSetCount=1; da.pSetLayouts=&descriptorLayout;
        check(vkAllocateDescriptorSets(device,&da,&descriptor),"allocate descriptor");
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,12};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount=1; pl.pSetLayouts=&descriptorLayout; pl.pushConstantRangeCount=1; pl.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device,&pl,nullptr,&pipelineLayout),"create pipeline layout");
        std::ifstream file(shaderPath,std::ios::binary|std::ios::ate); require(bool(file),"cannot open compute shader");
        auto length=file.tellg(); require(length>0 && static_cast<std::size_t>(length)%4==0,"invalid shader length");
        std::vector<std::uint32_t> code(static_cast<std::size_t>(length)/4); file.seekg(0); file.read(reinterpret_cast<char*>(code.data()),length); require(bool(file),"cannot read shader");
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=code.size()*4; sm.pCode=code.data();
        VkShaderModule shader{}; check(vkCreateShaderModule(device,&sm,nullptr,&shader),"create shader");
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pi.layout=pipelineLayout;
        pi.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; pi.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; pi.stage.module=shader; pi.stage.pName="main";
        VkResult result=vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline);
        vkDestroyShaderModule(device,shader,nullptr); check(result,"create compute pipeline");
    }
    template<class F> void submit(F&& record) {
        check(vkResetCommandPool(device,commands,0),"reset command pool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool=commands; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1;
        VkCommandBuffer command{}; check(vkAllocateCommandBuffers(device,&ai,&command),"allocate command");
        try {
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vkBeginCommandBuffer(command,&bi),"begin command"); record(command); check(vkEndCommandBuffer(command),"end command");
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&command;
            check(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE),"submit command"); check(vkQueueWaitIdle(queue),"wait queue");
        } catch(...) { vkFreeCommandBuffers(device,commands,1,&command); throw; }
        vkFreeCommandBuffers(device,commands,1,&command);
    }
    void compute(zvram::ManagedBufferPool::BufferView view,std::uint32_t salt,bool varying=false) {
        VkDescriptorBufferInfo info{view.buffer,0,view.size};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=descriptor; write.dstBinding=0; write.descriptorCount=1; write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo=&info;
        vkUpdateDescriptorSets(device,1,&write,0,nullptr);
        std::array<std::uint32_t,3> push{static_cast<std::uint32_t>(view.size/4),salt,varying?1u:0u};
        submit([&](VkCommandBuffer command) {
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
            vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&descriptor,0,nullptr);
            vkCmdPushConstants(command,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,12,push.data()); vkCmdDispatch(command,(push[0]+255)/256,1,1);
            barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        });
    }
    std::vector<std::uint32_t> directRead(zvram::ManagedBufferPool::BufferView view) {
        VkBuffer buffer{}; VkDeviceMemory allocation{};
        auto cleanup=[&] { if(buffer) vkDestroyBuffer(device,buffer,nullptr); if(allocation) vkFreeMemory(device,allocation,nullptr); };
        try {
            VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size=view.size; ci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            check(vkCreateBuffer(device,&ci,nullptr,&buffer),"create readback buffer"); VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device,buffer,&req);
            std::uint32_t type=UINT32_MAX;
            for(std::uint32_t i=0;i<memory.memoryTypeCount;++i) {
                auto flags=memory.memoryTypes[i].propertyFlags;
                if((req.memoryTypeBits&(1u<<i)) && (flags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) && !(flags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    if(type==UINT32_MAX) type=i;
                    if(flags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {type=i;break;}
                }
            }
            require(type!=UINT32_MAX,"no host readback memory");
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=type;
            check(vkAllocateMemory(device,&ai,nullptr,&allocation),"allocate readback memory"); check(vkBindBufferMemory(device,buffer,allocation,0),"bind readback");
            submit([&](VkCommandBuffer command) {
                VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&b,0,nullptr,0,nullptr);
                VkBufferCopy region{0,0,view.size}; vkCmdCopyBuffer(command,view.buffer,buffer,1,&region);
                b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
                vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&b,0,nullptr,0,nullptr);
            });
            std::vector<std::uint32_t> bytes(view.size/4); void* mapped{}; check(vkMapMemory(device,allocation,0,view.size,0,&mapped),"map readback");
            std::memcpy(bytes.data(),mapped,view.size); vkUnmapMemory(device,allocation); cleanup(); return bytes;
        } catch(...) {cleanup();throw;}
    }
};
} // namespace

int main(int argc,char** argv) try {
    const bool capacity=argc==3 && std::strcmp(argv[1],"--capacity-mib")==0;
    require(argc<=2 || capacity,"usage: zvram-managed-check [shader.spv] | --capacity-mib N");
    Context context; context.initialize(argc==2?argv[1]:ZVRAM_CHECK_SHADER_PATH);
    using Pool=zvram::ManagedBufferPool;
    if(capacity) {
        std::size_t parsed=0; const auto mib=std::stoull(argv[2],&parsed);
        require(parsed==std::strlen(argv[2]) && mib>=32 && mib<=40960 && mib%32==0,
                "capacity must be 32..40960 MiB in multiples of 32");
        Pool::Config config{context.physical,context.device,context.queue,context.family,256*MiB,64*MiB,8*MiB};
        Pool pool(config);
        std::vector<Pool::Id> ids;
        std::vector<std::uint32_t> payload(32*MiB/4);
        for(std::size_t i=0;i<mib/32;++i) {
            std::fill(payload.begin(),payload.end(),mix32(static_cast<std::uint32_t>(i+1)));
            auto id=pool.upload(payload.data(),payload.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            auto view=pool.acquire(id); context.compute(view,0x85ebca6bu); pool.release(id);
            ids.push_back(id);
        }
        for(std::size_t i=0;i<ids.size();++i) {
            const auto bytes=pool.readback(ids[i]);
            require(bytes.size()==payload.size()*4,"capacity readback size mismatch");
            const std::uint32_t expected=mix32(static_cast<std::uint32_t>(i+1))^0x85ebca6bu;
            for(std::size_t offset=0;offset<bytes.size();offset+=4) {
                std::uint32_t actual; std::memcpy(&actual,bytes.data()+offset,4);
                require(actual==expected,"capacity GPU mutation mismatch");
            }
            require(pool.evict(ids[i]),"cannot evict capacity buffer");
        }
        const auto statistics=pool.statistics();
        require(statistics.residentAllocationBytes<=config.residentBudget && statistics.hostStoredBytes<=config.hostBudget,
                "capacity pool exceeded its budgets");
        std::cout<<"PASS: "<<mib<<" MiB of highly compressible GPU-mutated data verified; resident budget=256 MiB"
                 <<" stored="<<statistics.hostStoredBytes<<" evictions="<<statistics.evictions
                 <<" restores="<<statistics.restores<<"\nThis synthetic pattern is not representative of model weights.\n";
        for(auto id:ids) require(pool.erase(id),"cannot erase capacity entry");
        require(pool.statistics().residentAllocationBytes==0 && pool.statistics().hostStoredBytes==0,"capacity cleanup leaked memory");
        return 0;
    }
    Pool::Config config{context.physical,context.device,context.queue,context.family,16*MiB,128*MiB,MiB};
    Pool pool(config);
    std::array<Pool::Id,8> ids{};
    std::array<std::uint32_t,8> mutations{};
    std::vector<std::uint32_t> payload(8*MiB/4);
    auto initial=[](std::size_t buffer,std::size_t word) { return buffer==0?7u:mix32(static_cast<std::uint32_t>(word)^static_cast<std::uint32_t>(buffer*0x9e3779b9u)); };
    auto withinBudget=[&] {
        auto statistics=pool.statistics();
        require(statistics.residentAllocationBytes<=config.residentBudget,"resident budget exceeded");
        require(statistics.hostStoredBytes<=config.hostBudget,"host store budget exceeded");
    };
    for(std::size_t i=0;i<ids.size();++i) {
        for(std::size_t j=0;j<payload.size();++j) payload[j]=initial(i,j);
        ids[i]=pool.upload(payload.data(),payload.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); withinBudget();
    }
    pool.acquire(ids[0]); pool.acquire(ids[1]);
    require(!pool.evict(ids[0]),"pinned buffer was evicted"); require(!pool.erase(ids[0]),"pinned buffer was erased");
    mustFail([&] {pool.acquire(ids[2]);},"all-pinned resident budget should refuse restore");
    pool.release(ids[0]); pool.release(ids[1]);
    for(std::uint32_t cycle=0;cycle<2;++cycle) {
        for(std::size_t i=0;i<ids.size();++i) {
            auto view=pool.acquire(ids[i]); withinBudget();
            std::uint32_t salt=0x85ebca6bu*(cycle+1)^static_cast<std::uint32_t>(i+17);
            context.compute(view,salt); mutations[i]^=salt; pool.release(ids[i]);
        }
        for(std::size_t i=0;i<ids.size();++i) {
            auto bytes=pool.readback(ids[i]); require(bytes.size()==payload.size()*4,"readback size mismatch");
            for(std::size_t j=0;j<payload.size();++j) {
                std::uint32_t actual; std::memcpy(&actual,bytes.data()+j*4,4);
                if(actual!=(initial(i,j)^mutations[i])) throw std::runtime_error("compute readback mismatch: buffer "+std::to_string(i)+" word "+std::to_string(j));
            }
            require(pool.evict(ids[i]),"unpinned buffer could not evict"); withinBudget();
        }
        std::cout<<"cycle "<<cycle+1<<": verified 64 MiB of GPU mutations after eviction/restoration\n";
    }
    auto statistics=pool.statistics(); require(statistics.evictions>0 && statistics.restores>0 && statistics.rawFallbacks>0,"eviction, restore, or raw fallback not exercised");
    std::cout<<"resident="<<statistics.residentAllocationBytes<<" stored="<<statistics.hostStoredBytes<<" evictions="<<statistics.evictions<<" restores="<<statistics.restores<<" raw-fallbacks="<<statistics.rawFallbacks<<'\n';
    for(auto id:ids) require(pool.erase(id),"cannot erase unpinned entry");
    require(pool.statistics().residentAllocationBytes==0 && pool.statistics().hostStoredBytes==0,"erase leaked accounted memory");

    // A dirty snapshot that exceeds the host limit must leave the GPU allocation intact.
    config.residentBudget=MiB; config.hostBudget=128; config.stagingChunkSize=4096;
    Pool limited(config); std::vector<std::uint32_t> zeros(1024,0);
    auto id=limited.upload(zeros.data(),4096,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto view=limited.acquire(id); context.compute(view,123,true); limited.release(id);
    mustFail([&] {limited.evict(id);},"incompressible dirty snapshot should exceed host limit");
    view=limited.acquire(id); auto current=context.directRead(view); limited.release(id);
    for(std::size_t i=0;i<current.size();++i) require(current[i]==mix32(static_cast<std::uint32_t>(i)^123u),"failed eviction lost dirty GPU data");
    require(limited.erase(id),"cannot clean failed eviction");
    std::vector<std::uint32_t> mixed(2048,0);
    for(std::size_t i=1024;i<mixed.size();++i) mixed[i]=mix32(static_cast<std::uint32_t>(i));
    id=limited.upload(mixed.data(),mixed.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    mustFail([&] {limited.evict(id);},"later incompressible chunk should exceed host limit");
    require(limited.statistics().hostStoredBytes==0,"failed partial eviction retained a host snapshot");
    view=limited.acquire(id); current=context.directRead(view); limited.release(id);
    require(current==mixed,"failed partial eviction lost GPU data");
    require(limited.erase(id),"cannot clean partial eviction");
    const std::array<std::uint8_t,3> tail{9,27,81};
    id=limited.upload(tail.data(),tail.size(),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    require(limited.evict(id),"cannot evict unaligned byte buffer");
    require(limited.readback(id)==std::vector<std::uint8_t>(tail.begin(),tail.end()),"unaligned byte restore mismatch");
    require(limited.erase(id),"cannot erase unaligned byte buffer");
    std::cout<<"PASS: compute integrity, LRU budgets, pinned refusal, raw fallback, and failed-eviction recovery\n";
    return 0;
} catch(const std::exception& error) { std::cerr<<"error: "<<error.what()<<'\n'; return 1; }
