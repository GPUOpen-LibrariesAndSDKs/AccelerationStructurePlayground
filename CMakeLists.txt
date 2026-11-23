cmake_minimum_required(VERSION 3.20)
project(AccelerationStructurePlayground)

# The playground needs C++20 (std::bit_cast, starts_with, defaulted operator==).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ASP_BUILD_VULKAN "Build the playground Vulkan backend and executable" ON)

# The playground's CPU math is GLM, so every configuration needs it, including the CPU-only one.
include(cmake/Glm.cmake)

add_subdirectory(external)

set(IMGUI_SOURCES
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
)

add_library(imgui_core STATIC ${IMGUI_SOURCES})
target_include_directories(imgui_core PUBLIC
    external/imgui
    external/imgui/backends
)
add_library(imgui_win32 STATIC external/imgui/backends/imgui_impl_win32.cpp)
target_link_libraries(imgui_win32 PUBLIC imgui_core)

if (ASP_BUILD_VULKAN)
    find_package(Vulkan REQUIRED)
    find_path(VOLK_INCLUDE_DIR volk.h HINTS "$ENV{VULKAN_SDK}/Include/Volk" REQUIRED)
    find_path(VMA_INCLUDE_DIR vma/vk_mem_alloc.h HINTS "$ENV{VULKAN_SDK}/Include" REQUIRED)
    add_library(volk STATIC "${VOLK_INCLUDE_DIR}/volk.c")
    target_include_directories(volk PUBLIC "${VOLK_INCLUDE_DIR}" "${Vulkan_INCLUDE_DIRS}")
    target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)
    target_link_libraries(volk PUBLIC Vulkan::Vulkan)

    add_library(imgui_vulkan STATIC external/imgui/backends/imgui_impl_vulkan.cpp)
    target_include_directories(imgui_vulkan PRIVATE "${VOLK_INCLUDE_DIR}" "${Vulkan_INCLUDE_DIRS}")
    target_compile_definitions(imgui_vulkan PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR
        IMGUI_IMPL_VULKAN_USE_VOLK)
    target_link_libraries(imgui_vulkan PUBLIC imgui_core PRIVATE volk)
endif()

# Warning level for first-party targets only. external/ does not compile clean at /W4 and is
# deliberately left alone.
function(asp_set_warnings target)
    if (MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

add_library(asp_core STATIC
    src/assets/gltf_asset.cpp
    src/assets/scene.cpp
    src/assets/gltf_state.cpp
    src/core/error.cpp
    src/core/image_write.cpp
    src/core/platform_win32.cpp
    src/options.cpp
    src/render/accel_struct_plan.cpp
    src/render/accel_struct_plan_validate.cpp
    src/render/accel_struct_runtime.cpp
    src/render/deform_plan.cpp
    src/render/backend_validation.cpp
    src/render/plan_format.cpp
    src/render/camera.cpp
    src/render/asset_layout.cpp
)
target_compile_features(asp_core PUBLIC cxx_std_20)
target_compile_definitions(asp_core PUBLIC NOMINMAX)
target_include_directories(asp_core PUBLIC src)
# glm is PUBLIC because core/math.h is a public header: every consumer of asp_core sees glm types.
target_link_libraries(asp_core PUBLIC nlohmann glm PRIVATE stb)
asp_set_warnings(asp_core)

# The GUI is backend agnostic: it reads diagnostics and capabilities and returns a request
# struct. Keeping it in its own library stops it being compiled once per consumer.
add_library(asp_gui STATIC src/gui.cpp)
target_link_libraries(asp_gui PUBLIC asp_core imgui_core)
asp_set_warnings(asp_gui)

if (ASP_BUILD_VULKAN)
    add_library(asp_platform_win32 STATIC
        src/core/gui_win32.cpp
        src/core/window.cpp
    )
    target_compile_definitions(asp_platform_win32 PRIVATE UNICODE _UNICODE)
    target_link_libraries(asp_platform_win32 PUBLIC asp_core PRIVATE imgui_win32)
    asp_set_warnings(asp_platform_win32)

    get_filename_component(VULKAN_SDK_ROOT "${VOLK_INCLUDE_DIR}" DIRECTORY)
    get_filename_component(VULKAN_SDK_ROOT "${VULKAN_SDK_ROOT}" DIRECTORY)
    find_program(ASP_DXC_EXECUTABLE dxc HINTS "${VULKAN_SDK_ROOT}/Bin" REQUIRED)
    set(ASP_SPIRV_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY "${ASP_SPIRV_DIR}")
    foreach(SHADER raytracing deform)
        if (SHADER STREQUAL "raytracing")
            set(SHADER_SOURCE raytracing_vulkan)
            set(SHADER_TARGET lib_6_6)
            set(SHADER_ENTRY_ARGS)
            set(SHADER_EXTRA_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/raytracing_common.hlsli")
        else()
            set(SHADER_SOURCE ${SHADER})
            set(SHADER_TARGET cs_6_6)
            set(SHADER_ENTRY_ARGS -E DeformMain)
            set(SHADER_EXTRA_DEPENDS)
        endif()
        add_custom_command(OUTPUT "${ASP_SPIRV_DIR}/${SHADER}.spv"
            COMMAND "${ASP_DXC_EXECUTABLE}" -spirv "-fspv-target-env=vulkan1.3" -fvk-use-scalar-layout
                -HV 2021 -O3 -T ${SHADER_TARGET} ${SHADER_ENTRY_ARGS}
                -I "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER_SOURCE}.hlsl"
                -Fo "${ASP_SPIRV_DIR}/${SHADER}.spv"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER_SOURCE}.hlsl"
                    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common.hlsli"
                    ${SHADER_EXTRA_DEPENDS}
            VERBATIM)
        list(APPEND ASP_SPIRV_OUTPUTS "${ASP_SPIRV_DIR}/${SHADER}.spv")
    endforeach()
    add_custom_target(asp_vulkan_shaders DEPENDS ${ASP_SPIRV_OUTPUTS})

    add_library(asp_vulkan STATIC
        src/backends/vulkan/backend.cpp
        src/backends/vulkan/core/gpu.cpp
        src/backends/vulkan/core/swapchain.cpp
        src/backends/vulkan/core/vma.cpp
        src/backends/vulkan/render/accel_struct.cpp
        src/backends/vulkan/render/deform.cpp
        src/backends/vulkan/render/raytracer.cpp
        src/backends/vulkan/render/asset_gpu.cpp
    )
    add_dependencies(asp_vulkan asp_vulkan_shaders)
    target_include_directories(asp_vulkan PRIVATE "${VMA_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/vulkan")
    target_compile_definitions(asp_vulkan PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR UNICODE _UNICODE
        ASP_SPIRV_DIR=L"${ASP_SPIRV_DIR}")
    target_link_libraries(asp_vulkan PRIVATE asp_core asp_platform_win32 imgui_vulkan volk Vulkan::Vulkan)
    asp_set_warnings(asp_vulkan)

    add_executable(AccelerationStructurePlayground
        src/app.cpp
        src/main.cpp
        src/render/backend.cpp
    )
    target_compile_definitions(AccelerationStructurePlayground PRIVATE UNICODE _UNICODE)
    target_link_libraries(AccelerationStructurePlayground PRIVATE asp_core asp_gui asp_platform_win32 imgui_core
        asp_vulkan)
    asp_set_warnings(AccelerationStructurePlayground)
    add_custom_command(TARGET AccelerationStructurePlayground POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${ASP_SPIRV_DIR}"
            "$<TARGET_FILE_DIR:AccelerationStructurePlayground>/shaders"
        VERBATIM)

endif()
