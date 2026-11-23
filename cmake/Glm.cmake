include_guard(GLOBAL)

# GLM is the CPU math library for the playground. It ships with the Vulkan SDK, so the common case
# needs no configuration, but find_path also honours CMAKE_PREFIX_PATH and an ASP_GLM_INCLUDE_DIR
# set on the command line for anyone who keeps GLM elsewhere.
#
# Note this makes a GLM include directory a requirement of every configuration, including the
# CPU-only build.
find_path(ASP_GLM_INCLUDE_DIR glm/glm.hpp HINTS "$ENV{VULKAN_SDK}/Include"
    DOC "Directory containing the glm/ header tree")
if (NOT ASP_GLM_INCLUDE_DIR)
    message(FATAL_ERROR
        "Could not find glm/glm.hpp. Install the Vulkan SDK (which bundles GLM) and set VULKAN_SDK, "
        "or point -DASP_GLM_INCLUDE_DIR=<dir> at a directory containing glm/.")
endif()

add_library(glm INTERFACE)
target_include_directories(glm INTERFACE "${ASP_GLM_INCLUDE_DIR}")
