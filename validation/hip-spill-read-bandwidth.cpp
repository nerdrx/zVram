#include <hip/hip_runtime.h>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
static void ok(hipError_t e){if(e!=hipSuccess){std::fprintf(stderr,"HIP: %s\n",hipGetErrorString(e));std::exit(1);}}
__global__ void read_all(const uint32_t* data,uint32_t* result,size_t count){
 __shared__ uint32_t sums[256];uint32_t sum=0;
 for(size_t i=blockIdx.x*blockDim.x+threadIdx.x;i<count;i+=gridDim.x*blockDim.x)sum+=data[i];
 sums[threadIdx.x]=sum;__syncthreads();
 for(unsigned stride=128;stride;stride/=2){if(threadIdx.x<stride)sums[threadIdx.x]+=sums[threadIdx.x+stride];__syncthreads();}
 if(threadIdx.x==0)result[blockIdx.x]=sums[0];
}
int main(){
 constexpr size_t bytes=256ull*1024*1024,count=bytes/4;constexpr int blocks=1024,iterations=32;
 std::vector<uint32_t> data(count),result(blocks);uint64_t expected=0;
 for(size_t i=0;i<count;i++){data[i]=i%1024;expected+=data[i];}
 uint32_t *input=nullptr,*output=nullptr;ok(hipMalloc(&input,bytes));ok(hipMalloc(&output,blocks*4));
 ok(hipMemcpy(input,data.data(),bytes,hipMemcpyHostToDevice));
 hipLaunchKernelGGL(read_all,dim3(blocks),dim3(256),0,0,input,output,count);ok(hipGetLastError());ok(hipDeviceSynchronize());
 hipEvent_t start,end;ok(hipEventCreate(&start));ok(hipEventCreate(&end));ok(hipEventRecord(start));
 for(int i=0;i<iterations;i++)hipLaunchKernelGGL(read_all,dim3(blocks),dim3(256),0,0,input,output,count);
 ok(hipGetLastError());ok(hipEventRecord(end));ok(hipEventSynchronize(end));float ms=0;ok(hipEventElapsedTime(&ms,start,end));
 ok(hipMemcpy(result.data(),output,blocks*4,hipMemcpyDeviceToHost));uint64_t actual=0;for(auto x:result)actual+=x;
 std::printf("{\"bytes_per_kernel\":%zu,\"iterations\":%d,\"gpu_ms\":%.6f,\"read_gb_per_s\":%.6f,\"checksum\":%llu,\"expected_checksum\":%llu,\"integrity\":%s}\n",bytes,iterations,ms,double(bytes)*iterations/(ms*1e6),(unsigned long long)actual,(unsigned long long)expected,actual==expected?"true":"false");
 ok(hipEventDestroy(start));ok(hipEventDestroy(end));ok(hipFree(output));ok(hipFree(input));return actual==expected?0:2;
}
