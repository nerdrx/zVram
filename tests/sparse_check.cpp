#include "managed_pool.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr VkDeviceSize MiB = 1024 * 1024;

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + ": " + std::to_string(result));
}
void require(bool value, const char* what) {
    if (!value) throw std::runtime_error(what);
}

struct Context {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    std::uint32_t family{};
    VkCommandPool commands{};
    VkDescriptorSetLayout descriptorLayout{};
    VkDescriptorPool descriptorPool{};
    VkDescriptorSet descriptor{};
    VkPipelineLayout pipelineLayout{};
    VkPipeline pipeline{};

    ~Context() {
        if (device) vkDeviceWaitIdle(device);
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        if (descriptorLayout) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
        if (commands) vkDestroyCommandPool(device, commands, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    void initialize(const char* shaderPath) {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");

        std::uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices(count)");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
            VkPhysicalDeviceFeatures features{};
            vkGetPhysicalDeviceFeatures(candidate, &features);
            if (!features.sparseBinding || !features.sparseResidencyBuffer) continue;
            std::uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
            for (std::uint32_t i = 0; i < familyCount; ++i) {
                const VkQueueFlags required = VK_QUEUE_SPARSE_BINDING_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT;
                if (families[i].queueCount && (families[i].queueFlags & required) == required) {
                    physical = candidate;
                    family = i;
                    break;
                }
            }
            if (physical) break;
        }
        require(physical != VK_NULL_HANDLE, "no discrete GPU supports sparse buffers on a compute/transfer queue");

        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures enabled{};
        enabled.sparseBinding = VK_TRUE;
        enabled.sparseResidencyBuffer = VK_TRUE;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.pEnabledFeatures = &enabled;
        check(vkCreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, family, 0, &queue);

        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cp.queueFamilyIndex = family;
        check(vkCreateCommandPool(device, &cp, nullptr, &commands), "vkCreateCommandPool");

        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = 1;
        dl.pBindings = &binding;
        check(vkCreateDescriptorSetLayout(device, &dl, nullptr, &descriptorLayout), "vkCreateDescriptorSetLayout");
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 1;
        dp.poolSizeCount = 1;
        dp.pPoolSizes = &poolSize;
        check(vkCreateDescriptorPool(device, &dp, nullptr, &descriptorPool), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = descriptorPool;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &descriptorLayout;
        check(vkAllocateDescriptorSets(device, &da, &descriptor), "vkAllocateDescriptorSets");

        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &descriptorLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device, &pl, nullptr, &pipelineLayout), "vkCreatePipelineLayout");
        std::ifstream file(shaderPath, std::ios::binary | std::ios::ate);
        require(bool(file), "cannot open compute shader");
        const auto length = file.tellg();
        require(length > 0 && static_cast<std::size_t>(length) % 4 == 0, "invalid shader length");
        std::vector<std::uint32_t> code(static_cast<std::size_t>(length) / 4);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(code.data()), length);
        require(bool(file), "cannot read compute shader");
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.codeSize = code.size() * sizeof(std::uint32_t);
        sm.pCode = code.data();
        VkShaderModule module{};
        check(vkCreateShaderModule(device, &sm, nullptr, &module), "vkCreateShaderModule");
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pi.layout = pipelineLayout;
        pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pi.stage.module = module;
        pi.stage.pName = "main";
        const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        check(result, "vkCreateComputePipelines");
    }

    void mutate(zvram::ManagedBufferPool::BufferView view, std::uint32_t salt) {
        VkDescriptorBufferInfo info{view.buffer, 0, view.size};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptor;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        const std::array<std::uint32_t, 3> push{
            static_cast<std::uint32_t>(view.size / sizeof(std::uint32_t)), salt, 0};

        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = commands;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd{};
        check(vkAllocateCommandBuffers(device, &ai, &cmd), "vkAllocateCommandBuffers");
        try {
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &barrier, 0, nullptr, 0, nullptr);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptor, 0, nullptr);
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, push.data());
            vkCmdDispatch(cmd, (push[0] + 255) / 256, 1, 1);
            check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
            check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
        } catch (...) {
            vkFreeCommandBuffers(device, commands, 1, &cmd);
            throw;
        }
        vkFreeCommandBuffers(device, commands, 1, &cmd);
    }
};
} // namespace

int main(int argc, char** argv) try {
    require(argc == 2, "usage: zvram-sparse-check shader.spv");
    Context context;
    context.initialize(argv[1]);

    zvram::ManagedBufferPool::Config config;
    config.physicalDevice = context.physical;
    config.device = context.device;
    config.queue = context.queue;
    config.queueFamily = context.family;
    config.residentBudget = 2 * MiB;
    config.hostBudget = 8 * MiB;
    config.stagingChunkSize = 256 * 1024;
    config.stableSparseBuffers = true;
    zvram::ManagedBufferPool pool(config);

    constexpr std::size_t BufferBytes = 1 * MiB;
    constexpr std::size_t BufferCount = 3;
    const std::array<std::uint32_t, BufferCount> initial{0x12345678u, 0x9abcdef0u, 0x5a5aa5a5u};
    std::array<zvram::ManagedBufferPool::Id, BufferCount> ids{};
    std::array<VkBuffer, BufferCount> stable{};
    std::array<std::uint32_t, BufferCount> expected = initial;
    std::vector<std::uint32_t> data(BufferBytes / sizeof(std::uint32_t));
    for (std::size_t i = 0; i < BufferCount; ++i) {
        std::fill(data.begin(), data.end(), initial[i]);
        ids[i] = pool.upload(data.data(), BufferBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }

    for (std::uint32_t cycle = 0; cycle < 2; ++cycle) {
        for (std::size_t i = 0; i < BufferCount; ++i) {
            const auto view = pool.acquire(ids[i]);
            if (!stable[i]) stable[i] = view.buffer;
            require(stable[i] == view.buffer, "sparse restore changed VkBuffer handle");
            const std::uint32_t salt = 0x1020304u * (cycle + 1) ^ static_cast<std::uint32_t>(i + 1);
            context.mutate(view, salt);
            expected[i] ^= salt;
            pool.release(ids[i]);
        }
        for (std::size_t i = 0; i < BufferCount; ++i) {
            const auto bytes = pool.readback(ids[i]);
            require(bytes.size() == BufferBytes, "sparse readback size mismatch");
            for (std::size_t offset = 0; offset < bytes.size(); offset += sizeof(std::uint32_t)) {
                std::uint32_t word{};
                std::memcpy(&word, bytes.data() + offset, sizeof(word));
                require(word == expected[i], "GPU-mutated sparse data changed across eviction");
            }
        }
        for (const auto id : ids) require(pool.evict(id), "could not evict sparse buffer");
    }

    const auto stats = pool.statistics();
    require(stats.evictions >= BufferCount * 2 && stats.restores > 0, "sparse eviction/restore path not exercised");
    for (const auto id : ids) require(pool.erase(id), "could not erase sparse buffer");
    const auto after = pool.statistics();
    require(after.residentAllocationBytes == 0 && after.hostStoredBytes == 0, "sparse pool cleanup leaked accounting");
    config.hostBudget = 128;
    {
        zvram::ManagedBufferPool limited(config);
        std::uint32_t random = 0x89172345u;
        for (auto& word : data) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            word = random;
        }
        const auto id = limited.upload(data.data(), BufferBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        const auto view = limited.acquire(id);
        require(!limited.evict(id) && !limited.erase(id), "pinned sparse backing was released");
        constexpr std::uint32_t salt = 0x5a617832u;
        context.mutate(view, salt);
        limited.release(id);
        bool refused = false;
        try { (void)limited.evict(id); } catch (const std::runtime_error&) { refused = true; }
        require(refused, "sparse eviction ignored the small host budget");
        require(limited.statistics().hostStoredBytes == 0, "failed sparse snapshot changed host accounting");
        const auto restored = limited.readback(id);
        for (std::size_t i = 0; i < data.size(); ++i) {
            std::uint32_t word = 0;
            std::memcpy(&word, restored.data() + i * 4, 4);
            require(word == (data[i] ^ salt), "failed sparse eviction lost GPU-mutated data");
        }
        require(limited.erase(id), "sparse failure-check cleanup failed");
        const std::array<std::uint8_t, 3> tiny{7, 8, 9};
        const auto edge = limited.upload(tiny.data(), tiny.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        const auto first = limited.acquire(edge);
        limited.release(edge);
        require(limited.evict(edge), "tiny sparse eviction failed");
        const auto second = limited.acquire(edge);
        require(first.buffer == second.buffer, "tiny sparse handle changed");
        limited.release(edge);
        require(limited.readback(edge) == std::vector<std::uint8_t>(tiny.begin(), tiny.end()), "tiny sparse readback failed");
        require(limited.erase(edge), "tiny sparse cleanup failed");
        require(limited.statistics().residentAllocationBytes == 0 && limited.statistics().hostStoredBytes == 0,
                "sparse edge/failure checks retained accounting");
    }
    std::cout << "PASS: stable sparse VkBuffer handles and GPU data survived two eviction/restore cycles; evictions="
              << stats.evictions << " restores=" << stats.restores
              << "; pinning, failed dirty eviction, and 3-byte alignment checked\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "sparse check: " << error.what() << '\n';
    return 1;
}
