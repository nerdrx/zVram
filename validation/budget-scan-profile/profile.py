#!/usr/bin/env python3
"""CPU-only baseline for the production residentAdmissionLimit scan."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "layer.cpp").read_text()
START = SOURCE.index("VkResult residentAdmissionLimit(Device& d,VkDeviceSize& limit) {")
OPEN = SOURCE.index("{", START)
depth = 0
for END in range(OPEN, len(SOURCE)):
    depth += (SOURCE[END] == "{") - (SOURCE[END] == "}")
    if depth == 0:
        END += 1
        break
helper = SOURCE[START:END]
HARNESS = r'''
#include <vulkan/vulkan.h>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <vector>
#include "resident_budget.hpp"
struct VirtualMemory {
    std::vector<VkDeviceMemory> children;
    std::vector<VkDeviceSize> childSizes;
    std::vector<std::uint32_t> childTypes;
};
struct Device {
    bool admissionBudgetSnapshotActive=false, admissionBudgetSnapshotValid=false;
    VkDeviceSize admissionBudgetSnapshotLimit=0, residentLimitBytes=500000000;
    VkDeviceSize budgetReserveBytes=10000000, lastBudgetLimit=UINT64_MAX;
    std::uint32_t budgetHeap=0;
    VkPhysicalDevice physical{};
    VkPhysicalDeviceMemoryProperties memory{};
    PFN_vkGetPhysicalDeviceMemoryProperties2 budgetProperties=nullptr;
    std::unordered_map<VkDeviceMemory,VirtualMemory> virtualMemory;
};
void logf(const char*, ...) {}
void budget(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2* p) {
    p->memoryProperties.memoryHeapCount=2;
    auto* b=static_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(p->pNext);
    b->heapBudget[0]=300000000; b->heapUsage[0]=250000000;
}
'''
HARNESS += helper + r'''
int main(int argc,char** argv) {
    const std::size_t count=argc>1?std::strtoull(argv[1],nullptr,10):3072;
    Device d; d.budgetProperties=budget;
    d.memory.memoryHeapCount=2; d.memory.memoryTypeCount=2;
    d.memory.memoryHeaps[0].size=300000000; d.memory.memoryHeaps[1].size=300000000;
    d.memory.memoryTypes[0].heapIndex=0; d.memory.memoryTypes[1].heapIndex=1;
    VirtualMemory m; m.children.resize(count); m.childSizes.assign(count,4096);
    m.childTypes.resize(count);
    for(std::size_t i=0;i<count;i++) {
        m.children[i]=reinterpret_cast<VkDeviceMemory>(i+1);
        m.childTypes[i]=static_cast<std::uint32_t>(i&1);
    }
    d.virtualMemory.emplace(reinterpret_cast<VkDeviceMemory>(1),std::move(m));
    const VkDeviceSize expected=std::min<VkDeviceSize>(d.residentLimitBytes,
        300000000-10000000-(250000000-(count/2)*4096));
    for(int sample=0;sample<5;sample++) {
        auto begin=std::chrono::steady_clock::now();
        for(int call=0;call<100;call++) {
            VkDeviceSize limit=0;
            if(residentAdmissionLimit(d,limit)!=VK_SUCCESS || limit!=expected) return 2;
        }
        auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-begin).count();
        std::printf("children=%zu sample=%d calls=100 ns/call=%.1f expected-limit=%llu\n",
            count,sample+1,ns/100.0,(unsigned long long)expected);
    }
}
'''
with tempfile.TemporaryDirectory() as td:
    cpp, exe = pathlib.Path(td) / "profile.cpp", pathlib.Path(td) / "profile"
    cpp.write_text(HARNESS.replace("#include \"resident_budget.hpp\"",
                                   f'#include "{ROOT / "resident_budget.hpp"}"'))
    subprocess.run(["c++", "-O2", "-std=c++17", str(cpp), "-o", str(exe)], check=True)
    for count in (3072, 100000):
        subprocess.run([str(exe), str(count)], check=True)
