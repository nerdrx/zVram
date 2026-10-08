cmake_minimum_required(VERSION 3.20)
project(zvram VERSION 0.2.0 LANGUAGES CXX)
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
option(ZVRAM_ENABLE_GDEFLATE "Build the experimental GDeflate snapshot codec and optional GPU restore" OFF)
option(ZVRAM_ENABLE_BP16_GPU_TESTS "Register opt-in BP16 GPU integrity tests" OFF)
set(ZVRAM_GDEFLATE_CAPABILITY false)
set(ZVRAM_GDEFLATE_GPU_CAPABILITY false)
configure_file(research/bp16/decode.spv bp16.spv COPYONLY)
configure_file(research/bp16/encode_analyze.spv bp16-encode-analyze.spv COPYONLY)
configure_file(research/bp16/encode_pack.spv bp16-encode-pack.spv COPYONLY)
if(ZVRAM_ENABLE_GDEFLATE)
    enable_language(C)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    include(FetchContent)
    FetchContent_Declare(zvram_libdeflate
        URL https://codeload.github.com/NVIDIA/libdeflate/tar.gz/8ba9502fb30d2bf728592d121f0d402e40c8cb05
        URL_HASH SHA256=d1b4c38dce43e68a5f4c28d0fbb3f81a01953039a3dea63f4bd1a84d7ff80592)
    FetchContent_MakeAvailable(zvram_libdeflate)
    find_program(ZVRAM_MAKE NAMES gmake make REQUIRED)
    add_custom_command(OUTPUT ${zvram_libdeflate_SOURCE_DIR}/libdeflate.a
        COMMAND ${ZVRAM_MAKE} -C ${zvram_libdeflate_SOURCE_DIR} -j2
                CC=${CMAKE_C_COMPILER} "CFLAGS=-O2 -fPIC" libdeflate.a
        VERBATIM)
    add_custom_target(zvram-libdeflate-build DEPENDS ${zvram_libdeflate_SOURCE_DIR}/libdeflate.a)
    add_library(zvram_gdeflate INTERFACE)
    add_dependencies(zvram_gdeflate zvram-libdeflate-build)
    target_include_directories(zvram_gdeflate INTERFACE ${zvram_libdeflate_SOURCE_DIR})
    target_compile_definitions(zvram_gdeflate INTERFACE ZVRAM_HAVE_GDEFLATE=1)
    target_link_libraries(zvram_gdeflate INTERFACE ${zvram_libdeflate_SOURCE_DIR}/libdeflate.a)
    set(ZVRAM_GDEFLATE_CAPABILITY true)
    set(ZVRAM_GDEFLATE_GPU_CAPABILITY true)
    configure_file(research/gdeflate/GDeflate-bounded-wave32.spv gdeflate-wave32.spv COPYONLY)
endif()
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/zvram-codecs-config.json
    CONTENT "{\"gdeflate\":${ZVRAM_GDEFLATE_CAPABILITY},\"gdeflate_gpu\":${ZVRAM_GDEFLATE_GPU_CAPABILITY},\"bp16\":true,\"bp16_gpu\":true}\n")
add_library(zvram_layer SHARED layer.cpp)
set_property(TARGET zvram_layer APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_CURRENT_BINARY_DIR}/zvram-codecs-config.json
    ${CMAKE_CURRENT_BINARY_DIR}/bp16.spv
    ${CMAKE_CURRENT_BINARY_DIR}/bp16-encode-analyze.spv
    ${CMAKE_CURRENT_BINARY_DIR}/bp16-encode-pack.spv)
add_custom_command(TARGET zvram_layer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_BINARY_DIR}/zvram-codecs-config.json ${CMAKE_CURRENT_BINARY_DIR}/zvram-codecs.json
    VERBATIM)
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
foreach(stage vert frag)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/graphics_check.${stage}.spv
        COMMAND ${ZVRAM_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/graphics_check.${stage}
                -o ${CMAKE_CURRENT_BINARY_DIR}/graphics_check.${stage}.spv
        DEPENDS graphics_check.${stage} VERBATIM)
endforeach()
add_executable(zvram-vulkan-graphics-check vulkan_graphics_check.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/graphics_check.vert.spv ${CMAKE_CURRENT_BINARY_DIR}/graphics_check.frag.spv)
target_compile_features(zvram-vulkan-graphics-check PRIVATE cxx_std_17)
target_compile_definitions(zvram-vulkan-graphics-check PRIVATE
    ZVRAM_GRAPHICS_VERT_PATH="${CMAKE_CURRENT_BINARY_DIR}/graphics_check.vert.spv"
    ZVRAM_GRAPHICS_FRAG_PATH="${CMAKE_CURRENT_BINARY_DIR}/graphics_check.frag.spv")
target_link_libraries(zvram-vulkan-graphics-check PRIVATE Vulkan::Vulkan Threads::Threads)
pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
if(SDL2_FOUND)
    target_compile_definitions(zvram-vulkan-graphics-check PRIVATE ZVRAM_GRAPHICS_SDL2=1)
    target_link_libraries(zvram-vulkan-graphics-check PRIVATE PkgConfig::SDL2)
endif()
add_executable(zvram-file-pool-check file_pool_check.cpp)
target_link_libraries(zvram-file-pool-check PRIVATE zvram_pool)
add_executable(zvram-compression-policy-check compression_policy_check.cpp)
target_compile_features(zvram-compression-policy-check PRIVATE cxx_std_17)
add_executable(zvram-clean-cache-policy-check clean_cache_policy_check.cpp)
target_compile_features(zvram-clean-cache-policy-check PRIVATE cxx_std_17)
add_executable(zvram-bp16-codec-check bp16_codec_check.cpp)
target_compile_features(zvram-bp16-codec-check PRIVATE cxx_std_17)
target_include_directories(zvram-bp16-codec-check PRIVATE ${Vulkan_INCLUDE_DIRS})
target_link_libraries(zvram-bp16-codec-check PRIVATE Threads::Threads)
add_executable(zvram-resident-bootstrap-check resident_bootstrap_check.cpp)
target_include_directories(zvram-resident-bootstrap-check PRIVATE ${Vulkan_INCLUDE_DIRS})
target_compile_features(zvram-resident-bootstrap-check PRIVATE cxx_std_17)
target_link_libraries(zvram-resident-bootstrap-check PRIVATE Threads::Threads PkgConfig::ZSTD)
add_executable(zvram-resident-budget-check resident_budget_check.cpp)
target_compile_features(zvram-resident-budget-check PRIVATE cxx_std_17)
enable_testing()
add_test(NAME cpu-bp16-codec COMMAND zvram-bp16-codec-check)
add_test(NAME cpu-resident-budget COMMAND zvram-resident-budget-check)
add_test(NAME cpu-resident-bootstrap COMMAND zvram-resident-bootstrap-check)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(graphics_check_launch ${CMAKE_CURRENT_SOURCE_DIR}/zvram --build-dir ${CMAKE_CURRENT_BINARY_DIR}
    --validate --isolate-layers --vulkan-virtual-mib 128 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 64)
add_test(NAME vulkan-graphics-cold-restore COMMAND ${graphics_check_launch}
    -- $<TARGET_FILE:zvram-vulkan-graphics-check>)
add_test(NAME vulkan-graphics-active-cold-restore COMMAND ${graphics_check_launch}
    --vulkan-selective-restore --vulkan-active-eviction -- $<TARGET_FILE:zvram-vulkan-graphics-check>)
add_test(NAME vulkan-graphics-native-allocation-cold-restore COMMAND ${graphics_check_launch}
    --vulkan-selective-restore --vulkan-active-eviction -- $<TARGET_FILE:zvram-vulkan-graphics-check> --native-allocation)
set_tests_properties(vulkan-graphics-cold-restore vulkan-graphics-active-cold-restore vulkan-graphics-native-allocation-cold-restore
    PROPERTIES TIMEOUT 45 LABELS "hardware;graphics")
add_test(NAME cpu-vulkan-ram-guard COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_vulkan_ram_guard.py)
set_tests_properties(cpu-vulkan-ram-guard PROPERTIES TIMEOUT 10)
add_test(NAME cpu-vulkan-compression-options COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_vulkan_compression_options.py)
set_tests_properties(cpu-vulkan-compression-options PROPERTIES TIMEOUT 10)
add_test(NAME cpu-vulkan-model-budget COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_vulkan_model_budget.py)
set_tests_properties(cpu-vulkan-model-budget PROPERTIES TIMEOUT 10)
add_test(NAME cpu-compression-policy COMMAND zvram-compression-policy-check)
add_test(NAME cpu-clean-cache-policy COMMAND zvram-clean-cache-policy-check)
add_executable(zvram-snapshot-decode-check snapshot_decode_check.cpp)
target_compile_features(zvram-snapshot-decode-check PRIVATE cxx_std_17)
target_link_libraries(zvram-snapshot-decode-check PRIVATE Threads::Threads PkgConfig::ZSTD)
add_test(NAME cpu-snapshot-decode COMMAND zvram-snapshot-decode-check)
add_executable(zvram-gdeflate-envelope-check gdeflate_envelope_check.cpp)
target_compile_features(zvram-gdeflate-envelope-check PRIVATE cxx_std_17)
add_test(NAME cpu-gdeflate-envelope COMMAND zvram-gdeflate-envelope-check)
option(ZVRAM_BUILD_GDEFLATE_RESEARCH "Build the standalone GDeflate research host (no production integration)" OFF)
if(ZVRAM_BUILD_GDEFLATE_RESEARCH)
    add_executable(zvram-gdeflate-research research/gdeflate/vulkan_gdeflate_smoke.cpp)
    target_compile_features(zvram-gdeflate-research PRIVATE cxx_std_17)
    target_include_directories(zvram-gdeflate-research PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(zvram-gdeflate-research PRIVATE Vulkan::Vulkan)
endif()
add_executable(zvram-snapshot-pipeline-check snapshot_pipeline_check.cpp)
target_compile_features(zvram-snapshot-pipeline-check PRIVATE cxx_std_17)
target_link_libraries(zvram-snapshot-pipeline-check PRIVATE Threads::Threads PkgConfig::ZSTD)
add_test(NAME cpu-snapshot-pipeline COMMAND zvram-snapshot-pipeline-check)
add_executable(zvram-submission-tracking-check submission_tracking_check.cpp)
target_compile_features(zvram-submission-tracking-check PRIVATE cxx_std_17)
target_include_directories(zvram-submission-tracking-check PRIVATE ${Vulkan_INCLUDE_DIRS})
add_test(NAME vulkan-submission-tracking COMMAND zvram-submission-tracking-check)
add_executable(zvram-active-refs-check active_refs_check.cpp)
target_compile_features(zvram-active-refs-check PRIVATE cxx_std_17)
target_include_directories(zvram-active-refs-check PRIVATE ${Vulkan_INCLUDE_DIRS})
add_test(NAME cpu-active-refs COMMAND zvram-active-refs-check)
add_test(NAME compression-roundtrip COMMAND zvram-compression-check)
add_test(NAME layer-capacity-integrity COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --isolate-layers -- $<TARGET_FILE:zvram-capacity-check> --mib 64)
add_test(NAME vulkan-virtual-single-integrity COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-mib 40960 -- $<TARGET_FILE:zvram-capacity-check> --single-allocation --mib 320 --chunk-mib 64)
add_test(NAME vulkan-virtual-api2-integrity COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-mib 98304 -- $<TARGET_FILE:zvram-capacity-check> --single-allocation --api2 --mib 320 --chunk-mib 64)
foreach(pool_api legacy api2)
    set(pool_flags --suballocation)
    if(pool_api STREQUAL "api2")
        list(APPEND pool_flags --suballocation-api2)
    endif()
    add_test(NAME vulkan-virtual-pool-${pool_api} COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-mib 512 -- $<TARGET_FILE:zvram-vulkan-auto-check> ${pool_flags})
    add_test(NAME vulkan-automatic-pool-${pool_api} COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-mib 512 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 512 -- $<TARGET_FILE:zvram-vulkan-auto-check> ${pool_flags} --suballocation-auto)
    set_tests_properties(vulkan-virtual-pool-${pool_api} vulkan-automatic-pool-${pool_api} PROPERTIES ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
endforeach()
add_test(NAME vulkan-automatic-pool-bda COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-mib 512 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 512 -- $<TARGET_FILE:zvram-vulkan-auto-check> --suballocation-auto --suballocation-api2 --bda)
set_tests_properties(vulkan-automatic-pool-bda PROPERTIES ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
foreach(native_pool_mode legacy api2 bda two-queues two-families)
    set(native_pool_flags --suballocation-auto --native-allocation)
    if(native_pool_mode STREQUAL "api2")
        list(APPEND native_pool_flags --suballocation-api2)
    elseif(native_pool_mode STREQUAL "bda")
        list(APPEND native_pool_flags --suballocation-api2 --bda)
    elseif(native_pool_mode STREQUAL "two-queues")
        list(APPEND native_pool_flags --two-queues)
    elseif(native_pool_mode STREQUAL "two-families")
        list(APPEND native_pool_flags --two-families --bda)
    endif()
    add_test(NAME vulkan-automatic-native-pool-${native_pool_mode} COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-mib 512 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 512 -- $<TARGET_FILE:zvram-vulkan-auto-check> ${native_pool_flags})
    set_tests_properties(vulkan-automatic-native-pool-${native_pool_mode} PROPERTIES ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
endforeach()
foreach(submit_mode synthetic-legacy synthetic-api2 native-legacy native-api2 synthetic-unknown native-unknown synthetic-bda native-bda)
    set(submit_flags --selective-submit)
    if(submit_mode MATCHES "api2$")
        list(APPEND submit_flags --selective-submit-api2)
    elseif(submit_mode MATCHES "unknown$")
        list(APPEND submit_flags --selective-submit-unknown)
    elseif(submit_mode MATCHES "bda$")
        list(APPEND submit_flags --bda)
    endif()
    if(submit_mode MATCHES "^native-")
        list(APPEND submit_flags --native-allocation)
    endif()
    add_test(NAME vulkan-selective-submit-${submit_mode}
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers
                --vulkan-virtual-mib 128 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 64 --vulkan-selective-restore --
                $<TARGET_FILE:zvram-vulkan-auto-check> ${submit_flags})
    set_tests_properties(vulkan-selective-submit-${submit_mode} PROPERTIES ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
endforeach()
foreach(range_mode synthetic native)
    set(range_flags --range-submit)
    if(range_mode STREQUAL "native")
        list(APPEND range_flags --native-allocation)
    endif()
    add_test(NAME vulkan-range-residency-${range_mode}
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers
            --vulkan-virtual-mib 128 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 64
            --vulkan-selective-restore --vulkan-active-eviction --vulkan-range-mib 32 --
            $<TARGET_FILE:zvram-vulkan-auto-check> ${range_flags})
    set_tests_properties(vulkan-range-residency-${range_mode} PROPERTIES TIMEOUT 60
        ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
endforeach()
foreach(range_mode synthetic native)
    set(range_flags --range-submit)
    if(range_mode STREQUAL "native")
        list(APPEND range_flags --native-allocation)
    endif()
    add_test(NAME vulkan-rawrange-${range_mode}
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers
            --vulkan-virtual-mib 128 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 128
            --vulkan-selective-restore --vulkan-active-eviction --vulkan-range-mib 32
            --vulkan-min-savings-percent 100 --
            $<TARGET_FILE:zvram-vulkan-auto-check> ${range_flags})
    set_tests_properties(vulkan-rawrange-${range_mode} PROPERTIES TIMEOUT 60
        ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
endforeach()
foreach(range_pressure_mode synthetic native)
    foreach(queue_mode single two-queues)
        set(range_pressure_flags --range-pressure)
        if(range_pressure_mode STREQUAL "native")
            list(APPEND range_pressure_flags --native-allocation)
        endif()
        if(queue_mode STREQUAL "two-queues")
            list(APPEND range_pressure_flags --two-queues)
        endif()
        set(test_name vulkan-range-pressure-${range_pressure_mode}-${queue_mode})
        add_test(NAME ${test_name}
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers
                --vulkan-virtual-mib 128 --vulkan-auto-idle-ms 60000 --vulkan-cold-mib 64
                --vulkan-selective-restore --vulkan-active-eviction --vulkan-range-mib 32
                --vulkan-resident-mib 32 --
                $<TARGET_FILE:zvram-vulkan-auto-check> ${range_pressure_flags})
        if(queue_mode STREQUAL "two-queues")
            set_tests_properties(${test_name} PROPERTIES TIMEOUT 60
                ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
        else()
            set_tests_properties(${test_name} PROPERTIES TIMEOUT 30
                ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
        endif()
    endforeach()
endforeach()
foreach(batch_mode synthetic native)
    set(batch_flags)
    if(batch_mode STREQUAL "native")
        list(APPEND batch_flags --native-allocation)
    endif()
    foreach(batch_savings 0 100)
        set(batch_test vulkan-transfer-batch-${batch_mode}-${batch_savings})
        add_test(NAME ${batch_test}
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers
                --vulkan-virtual-mib 512 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 512
                --vulkan-selective-restore --vulkan-active-eviction
                --vulkan-range-mib 128 --vulkan-min-savings-percent ${batch_savings} --
                $<TARGET_FILE:zvram-vulkan-auto-check> ${batch_flags})
        set_tests_properties(${batch_test} PROPERTIES TIMEOUT 30
            ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
    endforeach()
    foreach(shuffle_stride 2 4)
        set(shuffle_test vulkan-byte-shuffle-${batch_mode}-${shuffle_stride})
        add_test(NAME ${shuffle_test}
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers
                --vulkan-virtual-mib 512 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 512
                --vulkan-selective-restore --vulkan-active-eviction
                --vulkan-range-mib 128 --vulkan-byte-shuffle ${shuffle_stride} --
                $<TARGET_FILE:zvram-vulkan-auto-check> ${batch_flags})
        set_tests_properties(${shuffle_test} PROPERTIES TIMEOUT 30
            ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
    endforeach()
endforeach()
add_test(NAME vulkan-snapshot-pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_vulkan_pipeline.py
        ${CMAKE_CURRENT_SOURCE_DIR}/zvram $<TARGET_FILE:zvram-vulkan-auto-check>)
set_tests_properties(vulkan-snapshot-pipeline PROPERTIES TIMEOUT 480
    ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
foreach(range_cache_mode synthetic native)
    set(range_cache_flags --range-cache)
    if(range_cache_mode STREQUAL "native")
        list(APPEND range_cache_flags --native-allocation)
    endif()
    set(test_name vulkan-range-clean-cache-${range_cache_mode})
    add_test(NAME ${test_name}
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers
            --vulkan-virtual-mib 128 --vulkan-auto-idle-ms 60000 --vulkan-cold-mib 64
            --vulkan-selective-restore --vulkan-active-eviction --vulkan-range-mib 32
            --vulkan-resident-mib 32 --vulkan-clean-cache --
            $<TARGET_FILE:zvram-vulkan-auto-check> ${range_cache_flags})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 60
        ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
endforeach()
add_test(NAME vulkan-range-clean-cache-bootstrap
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_vulkan_cache_bootstrap.py
        ${CMAKE_CURRENT_SOURCE_DIR}/zvram $<TARGET_FILE:zvram-vulkan-auto-check>)
set_tests_properties(vulkan-range-clean-cache-bootstrap PROPERTIES TIMEOUT 60
    ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
foreach(range_cache_mode synthetic native)
    set(range_cache_flags --range-cache)
    if(range_cache_mode STREQUAL "native")
        list(APPEND range_cache_flags --native-allocation)
    endif()
    add_test(NAME vulkan-rawcache-${range_cache_mode}
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers
            --vulkan-virtual-mib 128 --vulkan-auto-idle-ms 60000 --vulkan-cold-mib 64
            --vulkan-selective-restore --vulkan-active-eviction --vulkan-range-mib 32
            --vulkan-resident-mib 32 --vulkan-clean-cache --vulkan-eviction-policy mru
            --vulkan-min-savings-percent 100 --
            $<TARGET_FILE:zvram-vulkan-auto-check> ${range_cache_flags})
    set_tests_properties(vulkan-rawcache-${range_cache_mode} PROPERTIES TIMEOUT 60
        ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
endforeach()
foreach(range_cache_mode synthetic native)
    set(range_cache_flags --range-cache-quota)
    if(range_cache_mode STREQUAL "native")
        list(APPEND range_cache_flags --native-allocation)
    endif()
    set(test_name vulkan-range-clean-cache-quota-${range_cache_mode})
    add_test(NAME ${test_name}
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers
            --vulkan-virtual-mib 128 --vulkan-auto-idle-ms 60000 --vulkan-cold-mib 64
            --vulkan-selective-restore --vulkan-active-eviction --vulkan-range-mib 32
            --vulkan-resident-mib 64 --vulkan-clean-cache --
            $<TARGET_FILE:zvram-vulkan-auto-check> ${range_cache_flags})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 60
        ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
endforeach()
foreach(range_cache_mode synthetic native)
    set(range_cache_flags --range-cache)
    if(range_cache_mode STREQUAL "native")
        list(APPEND range_cache_flags --native-allocation)
    endif()
    add_test(NAME vulkan-range-mru-${range_cache_mode}
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers
            --vulkan-virtual-mib 128 --vulkan-auto-idle-ms 60000 --vulkan-cold-mib 64
            --vulkan-selective-restore --vulkan-active-eviction --vulkan-range-mib 32
            --vulkan-resident-mib 32 --vulkan-clean-cache --vulkan-eviction-policy mru --
            $<TARGET_FILE:zvram-vulkan-auto-check> ${range_cache_flags})
    set_tests_properties(vulkan-range-mru-${range_cache_mode} PROPERTIES TIMEOUT 60
        ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")

    set(range_cache_quota_flags --range-cache-quota)
    if(range_cache_mode STREQUAL "native")
        list(APPEND range_cache_quota_flags --native-allocation)
    endif()
    add_test(NAME vulkan-range-mru-quota-${range_cache_mode}
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers
            --vulkan-virtual-mib 128 --vulkan-auto-idle-ms 60000 --vulkan-cold-mib 64
            --vulkan-selective-restore --vulkan-active-eviction --vulkan-range-mib 32
            --vulkan-resident-mib 64 --vulkan-clean-cache --vulkan-eviction-policy mru --
            $<TARGET_FILE:zvram-vulkan-auto-check> ${range_cache_quota_flags})
    set_tests_properties(vulkan-range-mru-quota-${range_cache_mode} PROPERTIES TIMEOUT 60
        ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
endforeach()
foreach(range_cache_mode synthetic native)
    set(range_cache_flags --range-cache-unknown)
    if(range_cache_mode STREQUAL "native")
        list(APPEND range_cache_flags --native-allocation)
    endif()
    set(test_name vulkan-range-clean-cache-accepted-unknown-${range_cache_mode})
    add_test(NAME ${test_name}
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers
            --vulkan-virtual-mib 128 --vulkan-auto-idle-ms 1000 --vulkan-cold-mib 64
            --vulkan-selective-restore --vulkan-active-eviction --vulkan-range-mib 32
            --vulkan-resident-mib 96 --vulkan-clean-cache --
            $<TARGET_FILE:zvram-vulkan-auto-check> ${range_cache_flags})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 60
        ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
endforeach()
foreach(robust_mode synthetic native)
    set(robust_flags --range-pressure --robust-core)
    if(robust_mode STREQUAL "native")
        list(APPEND robust_flags --native-allocation)
    endif()
    add_test(NAME vulkan-range-pressure-robust-${robust_mode}
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers
            --vulkan-virtual-mib 128 --vulkan-auto-idle-ms 60000 --vulkan-cold-mib 64
            --vulkan-selective-restore --vulkan-active-eviction --vulkan-range-mib 32
            --vulkan-resident-mib 32 --vulkan-strict-robustness --
            $<TARGET_FILE:zvram-vulkan-auto-check> ${robust_flags})
    set_tests_properties(vulkan-range-pressure-robust-${robust_mode} PROPERTIES TIMEOUT 60
        ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
endforeach()
foreach(active_mode synthetic native synthetic-unknown native-unknown)
    set(active_flags --active-submit --two-queues)
    if(active_mode MATCHES "^native")
        list(APPEND active_flags --native-allocation)
    endif()
    if(active_mode MATCHES "unknown$")
        list(APPEND active_flags --selective-submit-unknown)
    endif()
    add_test(NAME vulkan-active-eviction-${active_mode}
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers
            --vulkan-virtual-mib 128 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 64
            --vulkan-selective-restore --vulkan-active-eviction --
            $<TARGET_FILE:zvram-vulkan-auto-check> ${active_flags})
    set_tests_properties(vulkan-active-eviction-${active_mode} PROPERTIES TIMEOUT 30
        ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
endforeach()
foreach(selective_mode synthetic-legacy synthetic-api2 native-legacy native-api2)
    set(selective_flags --selective-bind)
    if(selective_mode MATCHES "api2$")
        list(APPEND selective_flags --selective-bind-api2)
    endif()
    if(selective_mode MATCHES "^native-")
        list(APPEND selective_flags --native-allocation)
    endif()
    add_test(NAME vulkan-automatic-selective-bind-${selective_mode}
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers
                --vulkan-virtual-mib 128 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 64 --
                $<TARGET_FILE:zvram-vulkan-auto-check> ${selective_flags})
    set_tests_properties(vulkan-automatic-selective-bind-${selective_mode}
        PROPERTIES ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~;ZVRAM_TEST_RESTORE_FAIL_AFTER_GROUPS=1")
endforeach()
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
add_test(NAME vulkan-virtual-concurrent-pending-wait COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/zvram --validate --isolate-layers --vulkan-virtual-gib 96 -- $<TARGET_FILE:zvram-vulkan-auto-check> --native-allocation --two-queues --pending-wait --concurrent-wait)
set_tests_properties(vulkan-automatic-native-two-queues vulkan-automatic-native-two-families vulkan-automatic-exclusive-families vulkan-virtual-pending-bind vulkan-virtual-concurrent-pending-wait PROPERTIES ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~" TIMEOUT 20)
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
if(ZVRAM_ENABLE_GDEFLATE)
    foreach(codec_target zvram_layer zvram-snapshot-decode-check zvram-snapshot-pipeline-check)
        target_link_libraries(${codec_target} PRIVATE zvram_gdeflate)
    endforeach()
    add_executable(zvram-gdeflate-codec-check gdeflate_codec_check.cpp)
    target_compile_features(zvram-gdeflate-codec-check PRIVATE cxx_std_17)
    target_link_libraries(zvram-gdeflate-codec-check PRIVATE zvram_gdeflate Threads::Threads)
    add_test(NAME cpu-gdeflate-codec COMMAND zvram-gdeflate-codec-check ${CMAKE_CURRENT_SOURCE_DIR}/research/gdeflate/fixtures)
    add_executable(zvram-gdeflate-snapshot-check gdeflate_snapshot_check.cpp)
    target_compile_features(zvram-gdeflate-snapshot-check PRIVATE cxx_std_17)
    target_link_libraries(zvram-gdeflate-snapshot-check PRIVATE zvram_gdeflate Threads::Threads PkgConfig::ZSTD)
    add_test(NAME cpu-gdeflate-snapshot COMMAND zvram-gdeflate-snapshot-check)
    set(gdeflate_gpu_launch ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_gdeflate_gpu_layer.py
        ${CMAKE_CURRENT_SOURCE_DIR}/zvram --build-dir ${CMAKE_CURRENT_BINARY_DIR}
        --validate --isolate-layers --vulkan-virtual-gib 96 --vulkan-auto-idle-ms 100
        --vulkan-cold-mib 512 --vulkan-codec gdeflate --vulkan-gdeflate-gpu)
    add_test(NAME vulkan-gdeflate-gpu-idle-integrity COMMAND ${gdeflate_gpu_launch} -- $<TARGET_FILE:zvram-vulkan-auto-check>)
    add_test(NAME vulkan-gdeflate-gpu-native-two-queues COMMAND ${gdeflate_gpu_launch} -- $<TARGET_FILE:zvram-vulkan-auto-check> --native-allocation --two-queues --pending-wait)
    add_test(NAME vulkan-gdeflate-gpu-partial-restore COMMAND ${gdeflate_gpu_launch} -- $<TARGET_FILE:zvram-vulkan-auto-check> --expect-partial-restore)
    set_tests_properties(vulkan-gdeflate-gpu-partial-restore PROPERTIES ENVIRONMENT "ZVRAM_TEST_RESTORE_FAIL_AFTER_GROUPS=1")
    foreach(gdeflate_range_mode synthetic native)
        set(gdeflate_range_flags --range-compressed)
        if(gdeflate_range_mode STREQUAL "native")
            list(APPEND gdeflate_range_flags --native-allocation)
        endif()
        add_test(NAME vulkan-gdeflate-gpu-range-${gdeflate_range_mode} COMMAND ${gdeflate_gpu_launch}
            --vulkan-selective-restore --vulkan-active-eviction --vulkan-range-mib 32
            -- $<TARGET_FILE:zvram-vulkan-auto-check> ${gdeflate_range_flags})
    endforeach()
    add_executable(zvram-gdeflate-codec-model research/gdeflate/check_codec_model.cpp)
    target_compile_features(zvram-gdeflate-codec-model PRIVATE cxx_std_17)
    target_link_libraries(zvram-gdeflate-codec-model PRIVATE zvram_gdeflate Threads::Threads PkgConfig::ZSTD)
endif()
if(ZVRAM_ENABLE_BP16_GPU_TESTS)
    set(bp16_gpu_launch ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_gdeflate_gpu_layer.py
        ${CMAKE_CURRENT_SOURCE_DIR}/zvram --build-dir ${CMAKE_CURRENT_BINARY_DIR}
        --validate --isolate-layers --vulkan-virtual-gib 96 --vulkan-auto-idle-ms 100
        --vulkan-cold-mib 512 --vulkan-codec bp16 --vulkan-bp16-gpu)
    add_test(NAME vulkan-bp16-gpu-idle-integrity COMMAND ${bp16_gpu_launch}
        -- $<TARGET_FILE:zvram-vulkan-auto-check>)
    add_test(NAME vulkan-bp16-gpu-native-two-queues COMMAND ${bp16_gpu_launch}
        -- $<TARGET_FILE:zvram-vulkan-auto-check> --native-allocation --two-queues --pending-wait)
    add_test(NAME vulkan-bp16-gpu-partial-restore COMMAND ${bp16_gpu_launch}
        -- $<TARGET_FILE:zvram-vulkan-auto-check> --expect-partial-restore)
    set_tests_properties(vulkan-bp16-gpu-partial-restore PROPERTIES
        ENVIRONMENT "ZVRAM_TEST_RESTORE_FAIL_AFTER_GROUPS=1;VK_LOADER_LAYERS_DISABLE=~implicit~")
    foreach(bp16_range_mode synthetic native)
        set(bp16_range_flags --range-compressed)
        if(bp16_range_mode STREQUAL "native")
            list(APPEND bp16_range_flags --native-allocation)
        endif()
        set(bp16_test vulkan-bp16-gpu-range-${bp16_range_mode})
        add_test(NAME ${bp16_test} COMMAND ${bp16_gpu_launch}
            --vulkan-selective-restore --vulkan-active-eviction --vulkan-range-mib 32
            -- $<TARGET_FILE:zvram-vulkan-auto-check> ${bp16_range_flags})
        set_tests_properties(${bp16_test} PROPERTIES TIMEOUT 60
            ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
    endforeach()
    add_test(NAME vulkan-bp16-gpu-zero-pattern COMMAND ${bp16_gpu_launch}
        --vulkan-selective-restore --vulkan-active-eviction --vulkan-range-mib 32
        -- $<TARGET_FILE:zvram-vulkan-auto-check> --range-compressed --zero-pattern)
    set_tests_properties(vulkan-bp16-gpu-zero-pattern PROPERTIES TIMEOUT 60
        ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
    foreach(bp16_pressure_mode synthetic native)
        set(bp16_pressure_flags --range-pressure)
        if(bp16_pressure_mode STREQUAL "native")
            list(APPEND bp16_pressure_flags --native-allocation)
        endif()
        set(bp16_test vulkan-bp16-gpu-pressure-${bp16_pressure_mode})
        add_test(NAME ${bp16_test} COMMAND ${bp16_gpu_launch}
            --vulkan-selective-restore --vulkan-active-eviction --vulkan-range-mib 32
            --vulkan-resident-mib 32 --vulkan-auto-idle-ms 60000 -- $<TARGET_FILE:zvram-vulkan-auto-check> ${bp16_pressure_flags})
        set_tests_properties(${bp16_test} PROPERTIES TIMEOUT 60
            ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
        set(local_owner_test vulkan-bp16-local-owner-${bp16_pressure_mode})
        add_test(NAME ${local_owner_test} COMMAND ${bp16_gpu_launch}
            --vulkan-selective-restore --vulkan-active-eviction --vulkan-range-mib 32
            --vulkan-resident-mib 32 --vulkan-auto-idle-ms 60000
            --vulkan-lazy-backing --vulkan-headroom-mib 64 --vulkan-strict-robustness
            -- $<TARGET_FILE:zvram-vulkan-auto-check> ${bp16_pressure_flags})
        set_tests_properties(${local_owner_test} PROPERTIES TIMEOUT 60
            ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~;ZVRAM_VULKAN_BP16_LOCAL_OWNER_MIB=64;ZVRAM_VULKAN_BP16_LOCAL_TOTAL_MIB=96;ZVRAM_VULKAN_BP16_GPU_ENCODE=1;ZVRAM_VULKAN_BP16_ALLOCATED_HOST_INPUT=1;ZVRAM_VULKAN_BP16_ALLOCATED_HOST_MIB=512;ZVRAM_VULKAN_GPU_PROFILE=1")
    endforeach()
endif()
get_property(ZVRAM_TESTS DIRECTORY PROPERTY TESTS)
set_tests_properties(${ZVRAM_TESTS} PROPERTIES TIMEOUT 60)
set_tests_properties(vulkan-automatic-partial-freeze vulkan-automatic-partial-restore-retry PROPERTIES TIMEOUT 180)
set_tests_properties(vulkan-graphics-cold-restore vulkan-graphics-active-cold-restore vulkan-graphics-native-allocation-cold-restore PROPERTIES TIMEOUT 45)
