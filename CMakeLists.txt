cmake_minimum_required(VERSION 3.20)
project(zvram VERSION 0.2.0 LANGUAGES CXX)
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
add_library(zvram_layer SHARED layer.cpp)
target_include_directories(zvram_layer PRIVATE ${Vulkan_INCLUDE_DIRS})
target_compile_features(zvram_layer PRIVATE cxx_std_17)
target_link_libraries(zvram_layer PRIVATE Threads::Threads PkgConfig::ZSTD)
target_link_options(zvram_layer PRIVATE -Wl,-Bsymbolic)
configure_file(layer.json.in ${CMAKE_CURRENT_BINARY_DIR}/VK_LAYER_NX_zvram.json @ONLY)
add_executable(zvram-compression-check compression_check.cpp)
target_compile_features(zvram-compression-check PRIVATE cxx_std_17)
target_link_libraries(zvram-compression-check PRIVATE Vulkan::Vulkan PkgConfig::ZSTD)
add_executable(zvram-capacity-check capacity_check.cpp)
target_compile_features(zvram-capacity-check PRIVATE cxx_std_17)
target_link_libraries(zvram-capacity-check PRIVATE Vulkan::Vulkan)
add_library(zvram_pool STATIC managed_pool.cpp)
target_compile_features(zvram_pool PUBLIC cxx_std_17)
target_include_directories(zvram_pool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zvram_pool PUBLIC Vulkan::Vulkan PkgConfig::ZSTD)
find_program(ZVRAM_GLSLC glslc REQUIRED)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/managed_check.spv
    COMMAND ${ZVRAM_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/managed_check.comp -o ${CMAKE_CURRENT_BINARY_DIR}/managed_check.spv
    DEPENDS managed_check.comp VERBATIM)
add_executable(zvram-managed-check managed_check.cpp ${CMAKE_CURRENT_BINARY_DIR}/managed_check.spv)
target_compile_definitions(zvram-managed-check PRIVATE ZVRAM_CHECK_SHADER_PATH="${CMAKE_CURRENT_BINARY_DIR}/managed_check.spv")
target_link_libraries(zvram-managed-check PRIVATE zvram_pool)
add_executable(zvram-sparse-check sparse_check.cpp ${CMAKE_CURRENT_BINARY_DIR}/managed_check.spv)
target_link_libraries(zvram-sparse-check PRIVATE zvram_pool)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vulkan_auto_bda.spv
    COMMAND ${ZVRAM_GLSLC} --target-env=vulkan1.2 ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_auto_bda.comp -o ${CMAKE_CURRENT_BINARY_DIR}/vulkan_auto_bda.spv
    DEPENDS vulkan_auto_bda.comp VERBATIM)
add_executable(zvram-vulkan-auto-check vulkan_auto_check.cpp ${CMAKE_CURRENT_BINARY_DIR}/managed_check.spv ${CMAKE_CURRENT_BINARY_DIR}/vulkan_auto_bda.spv)
target_compile_features(zvram-vulkan-auto-check PRIVATE cxx_std_17)
target_compile_definitions(zvram-vulkan-auto-check PRIVATE ZVRAM_CHECK_SHADER_PATH="${CMAKE_CURRENT_BINARY_DIR}/managed_check.spv")
target_compile_definitions(zvram-vulkan-auto-check PRIVATE ZVRAM_BDA_SHADER_PATH="${CMAKE_CURRENT_BINARY_DIR}/vulkan_auto_bda.spv")
target_link_libraries(zvram-vulkan-auto-check PRIVATE Vulkan::Vulkan Threads::Threads)
add_executable(zvram-file-pool-check file_pool_check.cpp)
target_link_libraries(zvram-file-pool-check PRIVATE zvram_pool)
enable_testing()
add_test(NAME compression-roundtrip COMMAND zvram-compression-check)
add_test(NAME layer-capacity-integrity COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --isolate-layers -- $<TARGET_FILE:zvram-capacity-check> --mib 64)
add_test(NAME vulkan-virtual-single-integrity COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-mib 40960 -- $<TARGET_FILE:zvram-capacity-check> --single-allocation --mib 320 --chunk-mib 64)
add_test(NAME vulkan-virtual-api2-integrity COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-mib 98304 -- $<TARGET_FILE:zvram-capacity-check> --single-allocation --api2 --mib 320 --chunk-mib 64)
add_test(NAME vulkan-automatic-idle-integrity COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-gib 96 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 512 -- $<TARGET_FILE:zvram-vulkan-auto-check>)
add_test(NAME vulkan-automatic-budget-refusal COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-mib 98304 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 1 -- $<TARGET_FILE:zvram-vulkan-auto-check> --expect-budget-refusal)
add_test(NAME vulkan-automatic-budget-release COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-mib 98304 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 14 -- $<TARGET_FILE:zvram-vulkan-auto-check> --expect-budget-release)
add_test(NAME vulkan-automatic-partial-freeze COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-gib 96 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 11 -- $<TARGET_FILE:zvram-vulkan-auto-check> --expect-partial-freeze)
add_test(NAME vulkan-automatic-partial-restore-retry COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-gib 96 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 512 -- $<TARGET_FILE:zvram-vulkan-auto-check> --expect-partial-restore)
set_tests_properties(vulkan-automatic-partial-freeze PROPERTIES ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
set_tests_properties(vulkan-automatic-partial-restore-retry PROPERTIES ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~;ZVRAM_TEST_RESTORE_FAIL_AFTER_GROUPS=1")
add_test(NAME vulkan-automatic-bda-integrity COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-gib 96 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 512 -- $<TARGET_FILE:zvram-vulkan-auto-check> --bda)
add_test(NAME vulkan-automatic-native-two-queues COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-gib 96 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 512 -- $<TARGET_FILE:zvram-vulkan-auto-check> --native-allocation --two-queues --pending-wait)
add_test(NAME vulkan-automatic-native-two-families COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-gib 96 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 512 -- $<TARGET_FILE:zvram-vulkan-auto-check> --native-allocation --two-families --bda)
add_test(NAME vulkan-automatic-exclusive-families COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-gib 96 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 512 -- $<TARGET_FILE:zvram-vulkan-auto-check> --exclusive-families --bda)
add_test(NAME vulkan-virtual-pending-bind COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-gib 96 -- $<TARGET_FILE:zvram-vulkan-auto-check> --native-allocation --two-queues --pending-bind)
set_tests_properties(vulkan-automatic-native-two-queues vulkan-automatic-native-two-families vulkan-automatic-exclusive-families vulkan-virtual-pending-bind PROPERTIES ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~" TIMEOUT 20)
add_test(NAME managed-compute-integrity COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --isolate-layers -- $<TARGET_FILE:zvram-managed-check>)
add_test(NAME managed-capacity-integrity COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --isolate-layers -- $<TARGET_FILE:zvram-managed-check> --capacity-mib 320)
add_test(NAME sparse-compute-integrity COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers -- $<TARGET_FILE:zvram-sparse-check> ${CMAKE_CURRENT_BINARY_DIR}/managed_check.spv)
set_tests_properties(compression-roundtrip layer-capacity-integrity vulkan-virtual-single-integrity vulkan-virtual-api2-integrity vulkan-automatic-idle-integrity vulkan-automatic-budget-refusal vulkan-automatic-bda-integrity managed-compute-integrity managed-capacity-integrity sparse-compute-integrity PROPERTIES ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")

option(ZVRAM_BUILD_HIP "Build the experimental HIP spillover backend when ROCm is installed" ON)
set(ZVRAM_HIP_ARCH "gfx1100" CACHE STRING "AMD target for the HIP integration checks")
if(ZVRAM_BUILD_HIP)
    find_program(ZVRAM_HIPCC hipcc HINTS /opt/rocm/bin)
    find_path(ZVRAM_HIP_INCLUDE hip/hip_runtime_api.h HINTS /opt/rocm/include)
    find_library(ZVRAM_HIP_RUNTIME amdhip64 HINTS /opt/rocm/lib)
    if(ZVRAM_HIPCC AND ZVRAM_HIP_INCLUDE AND ZVRAM_HIP_RUNTIME)
        add_library(zvram_hip SHARED hip_layer.cpp hip_activity.cpp)
        include(CheckCXXSourceCompiles)
        set(CMAKE_REQUIRED_INCLUDES ${ZVRAM_HIP_INCLUDE})
        set(CMAKE_REQUIRED_DEFINITIONS -D__HIP_PLATFORM_AMD__=1)
        check_cxx_source_compiles("#include <GL/gl.h>
            #include <hip/hip_runtime.h>
            #include <hip/hip_deprecated.h>
            #include <hip/hip_gl_interop.h>
            #include <hip/amd_detail/hip_api_trace.hpp>
            static_assert(HIP_VERSION_MAJOR == 7 && HIP_VERSION_MINOR == 2 && HIP_VERSION_PATCH == 53211);
            static_assert(HIP_RUNTIME_API_TABLE_MAJOR_VERSION == 0 && HIP_RUNTIME_API_TABLE_STEP_VERSION == 18);
            static_assert(sizeof(HipDispatchTable) == 4056);
            int main() { return 0; }" ZVRAM_CAN_HIP_DISPATCH)
        unset(CMAKE_REQUIRED_INCLUDES)
        unset(CMAKE_REQUIRED_DEFINITIONS)
        if(ZVRAM_CAN_HIP_DISPATCH)
            target_sources(zvram_hip PRIVATE hip_dispatch.cpp)
            target_compile_definitions(zvram_hip PRIVATE ZVRAM_HAS_HIP_DISPATCH=1)
        else()
            message(STATUS "HIP dispatch ABI unsupported: automatic hibernation unavailable")
        endif()
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/hip_snapshot_copy.o
            COMMAND ${ZVRAM_HIPCC} --offload-arch=${ZVRAM_HIP_ARCH} -O2 -std=c++17 -fPIC -c
                ${CMAKE_CURRENT_SOURCE_DIR}/hip_snapshot_copy.cpp -o ${CMAKE_CURRENT_BINARY_DIR}/hip_snapshot_copy.o
            DEPENDS hip_snapshot_copy.cpp VERBATIM)
        set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/hip_snapshot_copy.o
            PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
        target_sources(zvram_hip PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/hip_snapshot_copy.o)
        target_include_directories(zvram_hip PRIVATE ${ZVRAM_HIP_INCLUDE})
        target_compile_features(zvram_hip PRIVATE cxx_std_17)
        target_compile_definitions(zvram_hip PRIVATE __HIP_PLATFORM_AMD__=1)
        target_link_libraries(zvram_hip PRIVATE ${ZVRAM_HIP_RUNTIME} ${CMAKE_DL_LIBS} Threads::Threads PkgConfig::ZSTD)
        pkg_check_modules(DRM_AMDGPU QUIET IMPORTED_TARGET libdrm_amdgpu)
        if(DRM_AMDGPU_FOUND)
            target_compile_definitions(zvram_hip PRIVATE ZVRAM_HAS_DRM_VMM=1)
            target_link_libraries(zvram_hip PRIVATE PkgConfig::DRM_AMDGPU)
            add_executable(zvram-hip-vmm-fd-check hip_vmm_fd_check.cpp)
            target_include_directories(zvram-hip-vmm-fd-check PRIVATE ${ZVRAM_HIP_INCLUDE})
            target_compile_features(zvram-hip-vmm-fd-check PRIVATE cxx_std_17)
            target_compile_definitions(zvram-hip-vmm-fd-check PRIVATE __HIP_PLATFORM_AMD__=1)
            target_link_libraries(zvram-hip-vmm-fd-check PRIVATE ${ZVRAM_HIP_RUNTIME})
        else()
            message(STATUS "libdrm_amdgpu development files absent: hybrid HIP VMM unavailable")
        endif()
        add_library(zvram_hip_oom_test SHARED test_hip_oom.cpp)
        target_include_directories(zvram_hip_oom_test PRIVATE ${ZVRAM_HIP_INCLUDE})
        target_compile_definitions(zvram_hip_oom_test PRIVATE __HIP_PLATFORM_AMD__=1)
        target_link_libraries(zvram_hip_oom_test PRIVATE ${CMAKE_DL_LIBS})
        add_executable(zvram-hip-capacity-check hip_capacity_check.cpp)
        target_include_directories(zvram-hip-capacity-check PRIVATE ${ZVRAM_HIP_INCLUDE})
        target_compile_definitions(zvram-hip-capacity-check PRIVATE __HIP_PLATFORM_AMD__=1)
        target_link_libraries(zvram-hip-capacity-check PRIVATE ${ZVRAM_HIP_RUNTIME})
        add_executable(zvram-hip-proc-check hip_proc_check.cpp)
        target_include_directories(zvram-hip-proc-check PRIVATE ${ZVRAM_HIP_INCLUDE})
        target_compile_features(zvram-hip-proc-check PRIVATE cxx_std_17)
        target_compile_definitions(zvram-hip-proc-check PRIVATE __HIP_PLATFORM_AMD__=1)
        target_link_libraries(zvram-hip-proc-check PRIVATE ${ZVRAM_HIP_RUNTIME} ${CMAKE_DL_LIBS})
        add_executable(zvram-hip-error-check hip_error_check.cpp)
        target_include_directories(zvram-hip-error-check PRIVATE ${ZVRAM_HIP_INCLUDE})
        target_compile_features(zvram-hip-error-check PRIVATE cxx_std_17)
        target_compile_definitions(zvram-hip-error-check PRIVATE __HIP_PLATFORM_AMD__=1)
        target_link_libraries(zvram-hip-error-check PRIVATE ${ZVRAM_HIP_RUNTIME} Threads::Threads)
        foreach(check hip_check hip_probe vmm_probe hip_hibernate_check hip_auto_check hip_auto_thread_check hip_auto_guard_check)
            add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${check}
                COMMAND ${ZVRAM_HIPCC} --offload-arch=${ZVRAM_HIP_ARCH} -O2 -std=c++17 -pthread
                    ${CMAKE_CURRENT_SOURCE_DIR}/${check}.cpp -ldl -o ${CMAKE_CURRENT_BINARY_DIR}/${check}
                DEPENDS ${check}.cpp VERBATIM)
            add_custom_target(zvram-${check} ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${check})
        endforeach()
        find_library(ZVRAM_HSA_RUNTIME hsa-runtime64 HINTS /opt/rocm/lib)
        if(ZVRAM_HSA_RUNTIME)
            get_filename_component(ZVRAM_HSA_LIBRARY_DIR ${ZVRAM_HSA_RUNTIME} DIRECTORY)
            foreach(check hsa_vmm_probe ram_vmm_probe)
                add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${check}
                    COMMAND ${ZVRAM_HIPCC} --offload-arch=${ZVRAM_HIP_ARCH} -O2 -std=c++17
                        ${CMAKE_CURRENT_SOURCE_DIR}/${check}.cpp -L${ZVRAM_HSA_LIBRARY_DIR} -lhsa-runtime64
                        -o ${CMAKE_CURRENT_BINARY_DIR}/${check}
                    DEPENDS ${check}.cpp VERBATIM)
                add_custom_target(zvram-${check} ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${check})
            endforeach()
        endif()
        add_test(NAME hip-native-integrity COMMAND ${CMAKE_CURRENT_BINARY_DIR}/hip_check)
        add_test(NAME hip-spill-integrity COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --hip --hip-local-mib 16 --hip-host-mib 64 -- ${CMAKE_CURRENT_BINARY_DIR}/hip_check)
        add_test(NAME hip-procedure-native-query COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --hip --hip-local-mib 32 --hip-host-mib 64 -- $<TARGET_FILE:zvram-hip-proc-check> --expect-physical)
        if(DRM_AMDGPU_FOUND)
        if(ZVRAM_CAN_HIP_DISPATCH)
        add_test(NAME hip-automatic-idle-integrity COMMAND ${CMAKE_COMMAND} -E env "HIP_VISIBLE_DEVICES=0"
            ${CMAKE_CURRENT_SOURCE_DIR}/zvram --hip --hip-vmm --hip-local-mib 16 --hip-host-mib 64
            --hip-auto-idle-ms 100 --hip-cold-mib 32 -- ${CMAKE_CURRENT_BINARY_DIR}/hip_auto_check)
        add_test(NAME hip-automatic-concurrent-integrity COMMAND ${CMAKE_COMMAND} -E env "HIP_VISIBLE_DEVICES=0"
            ${CMAKE_CURRENT_SOURCE_DIR}/zvram --hip --hip-vmm --hip-local-mib 16 --hip-host-mib 128
            --hip-auto-idle-ms 100 --hip-cold-mib 64 -- ${CMAKE_CURRENT_BINARY_DIR}/hip_auto_thread_check)
        add_test(NAME hip-automatic-local-integrity COMMAND ${CMAKE_COMMAND} -E env "HIP_VISIBLE_DEVICES=0"
            ${CMAKE_CURRENT_SOURCE_DIR}/zvram --hip --hip-vmm --hip-local-mib 64 --hip-host-mib 0
            --hip-auto-idle-ms 100 --hip-cold-mib 32 -- ${CMAKE_CURRENT_BINARY_DIR}/hip_auto_check)
        foreach(guard capture callback)
            if(guard STREQUAL "callback")
                set(ZVRAM_GUARD_ARGS --callback)
            else()
                set(ZVRAM_GUARD_ARGS)
            endif()
            add_test(NAME hip-automatic-${guard}-guard COMMAND ${CMAKE_COMMAND} -E env "HIP_VISIBLE_DEVICES=0"
                ${CMAKE_CURRENT_SOURCE_DIR}/zvram --hip --hip-vmm --hip-local-mib 16 --hip-host-mib 64
                --hip-auto-idle-ms 100 --hip-cold-mib 32 -- ${CMAKE_CURRENT_BINARY_DIR}/hip_auto_guard_check ${ZVRAM_GUARD_ARGS})
        endforeach()
        endif()
        add_test(NAME hip-hibernation-integrity COMMAND ${CMAKE_COMMAND} -E env "HIP_VISIBLE_DEVICES=0"
            ${CMAKE_CURRENT_SOURCE_DIR}/zvram --hip --hip-vmm --hip-local-mib 16 --hip-host-mib 64 --
            ${CMAKE_CURRENT_BINARY_DIR}/hip_hibernate_check)
        add_test(NAME hip-hibernation-multisegment COMMAND ${CMAKE_COMMAND} -E env "HIP_VISIBLE_DEVICES=0"
            ${CMAKE_CURRENT_SOURCE_DIR}/zvram --hip --hip-vmm --hip-local-mib 16 --hip-host-mib 768 --
            ${CMAKE_CURRENT_BINARY_DIR}/hip_hibernate_check --large)
        add_test(NAME hip-vmm-integrity COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --hip --hip-vmm --hip-local-mib 32 --hip-host-mib 32 -- ${CMAKE_CURRENT_BINARY_DIR}/hip_check --single-allocation --mib 64)
        add_test(NAME hip-vmm-host-integrity COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --hip --hip-vmm --hip-local-mib 0 --hip-host-mib 64 -- ${CMAKE_CURRENT_BINARY_DIR}/hip_check --single-allocation --mib 32)
        add_test(NAME hip-native-capacity-query COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --hip --hip-vmm --hip-local-mib 16 --hip-host-mib 64 -- $<TARGET_FILE:zvram-hip-capacity-check>)
        add_test(NAME hip-virtual-capacity-query COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --hip --hip-vmm --hip-report-capacity --hip-local-mib 16 --hip-host-mib 64 -- $<TARGET_FILE:zvram-hip-capacity-check>)
        add_test(NAME hip-vmm-descriptor-lifetime COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --hip --hip-vmm --hip-local-mib 0 --hip-host-mib 64 -- $<TARGET_FILE:zvram-hip-vmm-fd-check>)
        add_test(NAME hip-procedure-vmm-query COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --hip --hip-vmm --hip-report-capacity --hip-local-mib 16 --hip-host-mib 64 -- $<TARGET_FILE:zvram-hip-proc-check>)
        endif()
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME hip-fallback-error-state COMMAND ${CMAKE_COMMAND} -E env
            "LD_PRELOAD=$<TARGET_FILE:zvram_hip_oom_test>" "ZVRAM_TEST_NATIVE_ERROR=1" "HIP_VISIBLE_DEVICES=0"
            --unset=ZVRAM_HIP_LOCAL_LIMIT_MIB --unset=ZVRAM_HIP_VMM
            --unset=ZVRAM_HIP_REPORT_CAPACITY --unset=ZVRAM_TEST_VMM_FAIL_AFTER
            ${CMAKE_CURRENT_SOURCE_DIR}/zvram --hip --hip-host-mib 64 -- $<TARGET_FILE:zvram-hip-error-check>)
        if(DRM_AMDGPU_FOUND)
        add_test(NAME hip-vmm-error-state COMMAND ${CMAKE_COMMAND} -E env
            "LD_PRELOAD=$<TARGET_FILE:zvram_hip_oom_test>" "ZVRAM_TEST_NATIVE_ERROR=1" "HIP_VISIBLE_DEVICES=0"
            --unset=ZVRAM_TEST_VMM_FAIL_AFTER
            ${CMAKE_CURRENT_SOURCE_DIR}/zvram --hip --hip-vmm --hip-report-capacity
            --hip-local-mib 16 --hip-host-mib 64 -- $<TARGET_FILE:zvram-hip-error-check>)
        endif()
        add_test(NAME hip-injected-oom-integrity COMMAND ${Python3_EXECUTABLE} -c
            "import os,subprocess; e=os.environ.copy(); e['LD_PRELOAD']='${CMAKE_CURRENT_BINARY_DIR}/libzvram_hip_oom_test.so'; e.pop('ZVRAM_HIP_LOCAL_LIMIT_MIB',None); e.pop('ZVRAM_HIP_VMM',None); p=subprocess.run(['${CMAKE_CURRENT_SOURCE_DIR}/zvram','--hip','--hip-host-mib','64','--','${CMAKE_CURRENT_BINARY_DIR}/hip_check'],env=e,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True); print(p.stdout); assert p.returncode==0; assert 'mapped_host_allocations=6 ' in p.stdout")
        if(DRM_AMDGPU_FOUND)
        add_test(NAME hip-hibernation-remap-recovery COMMAND ${CMAKE_COMMAND} -E env
            "LD_PRELOAD=$<TARGET_FILE:zvram_hip_oom_test>" "HIP_VISIBLE_DEVICES=0"
            "ZVRAM_TEST_VMM_FAIL_AFTER=999999" "ZVRAM_TEST_VMM_MAP_FAIL_AFTER=3"
            --unset=ZVRAM_TEST_NATIVE_ERROR
            ${CMAKE_CURRENT_SOURCE_DIR}/zvram --hip --hip-vmm --hip-local-mib 16 --hip-host-mib 64 --
            ${CMAKE_CURRENT_BINARY_DIR}/hip_hibernate_check --retry-map-failure)
        add_test(NAME hip-vmm-partial-failure-cleanup COMMAND ${Python3_EXECUTABLE} -c
            "import os,subprocess; e=os.environ.copy(); e['LD_PRELOAD']='${CMAKE_CURRENT_BINARY_DIR}/libzvram_hip_oom_test.so'; e['ZVRAM_TEST_VMM_FAIL_AFTER']='1'; p=subprocess.run(['${CMAKE_CURRENT_SOURCE_DIR}/zvram','--hip','--hip-vmm','--hip-local-mib','512','--hip-host-mib','256','--','${CMAKE_CURRENT_BINARY_DIR}/hip_check','--single-allocation','--mib','768'],env=e,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True); print(p.stdout); assert p.returncode==1; assert 'failures=1' in p.stdout; assert 'hipMalloc_api_bytes_current=0' in p.stdout; assert 'host_pinned_backing_bytes_current=0 pending=0' in p.stdout; assert 'tracked=0 ' in p.stdout")
        endif()
    else()
        message(STATUS "ROCm development tools absent: skipping HIP backend")
    endif()
endif()
get_property(ZVRAM_TESTS DIRECTORY PROPERTY TESTS)
set_tests_properties(${ZVRAM_TESTS} PROPERTIES TIMEOUT 60)
set_tests_properties(vulkan-automatic-partial-freeze vulkan-automatic-partial-restore-retry PROPERTIES TIMEOUT 180)
