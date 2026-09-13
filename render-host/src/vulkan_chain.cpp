// Shared pNext-chain node sizing, compiled into BOTH processes so they
// can never disagree about a struct's length. See vulkan_forward.h.
//
// Sizes come from the real Vulkan headers of whichever process is
// compiling, which is exactly what each side needs when it copies into
// its own structs. These are spec-fixed layouts, so the two agree; the
// wire also carries the size so a genuine mismatch is caught rather than
// silently misread.

#include "stud/vulkan_forward.h"

#include <vulkan/vulkan_core.h>

namespace stud::render_host::vk_wire {

uint32_t chain_node_size(uint32_t s_type) {
    switch (static_cast<VkStructureType>(s_type)) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
            return sizeof(VkPhysicalDeviceFeatures2);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
            return sizeof(VkPhysicalDeviceVulkan11Features);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
            return sizeof(VkPhysicalDeviceVulkan12Features);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
            return sizeof(VkPhysicalDeviceVulkan13Features);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
            return sizeof(VkPhysicalDevice16BitStorageFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES:
            return sizeof(VkPhysicalDevice8BitStorageFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
            return sizeof(VkPhysicalDeviceMultiviewFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
            return sizeof(VkPhysicalDeviceSamplerYcbcrConversionFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
            return sizeof(VkPhysicalDeviceShaderDrawParametersFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
            return sizeof(VkPhysicalDeviceVariablePointersFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
            return sizeof(VkPhysicalDeviceProtectedMemoryFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
            return sizeof(VkPhysicalDeviceDescriptorIndexingFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
            return sizeof(VkPhysicalDeviceTimelineSemaphoreFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
            return sizeof(VkPhysicalDeviceBufferDeviceAddressFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:
            return sizeof(VkPhysicalDeviceHostQueryResetFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
            return sizeof(VkPhysicalDeviceScalarBlockLayoutFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES:
            return sizeof(VkPhysicalDeviceUniformBufferStandardLayoutFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES:
            return sizeof(VkPhysicalDeviceImagelessFramebufferFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES:
            return sizeof(VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
            return sizeof(VkPhysicalDeviceShaderFloat16Int8Features);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES:
            return sizeof(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES:
            return sizeof(VkPhysicalDeviceDynamicRenderingFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES:
            return sizeof(VkPhysicalDeviceSynchronization2Features);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES:
            return sizeof(VkPhysicalDeviceMaintenance4Features);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT:
            return sizeof(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT:
            return sizeof(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT:
            return sizeof(VkPhysicalDeviceCustomBorderColorFeaturesEXT);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT:
            return sizeof(VkPhysicalDeviceIndexTypeUint8FeaturesEXT);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT:
            return sizeof(VkPhysicalDeviceLineRasterizationFeaturesEXT);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
            return sizeof(VkPhysicalDeviceRobustness2FeaturesEXT);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT:
            return sizeof(VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES:
            return sizeof(VkPhysicalDevicePipelineCreationCacheControlFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES:
            return sizeof(VkPhysicalDeviceTextureCompressionASTCHDRFeatures);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR:
            return sizeof(VkPhysicalDeviceFragmentShadingRateFeaturesKHR);
        case VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR:
            return sizeof(VkDeviceQueueGlobalPriorityCreateInfoKHR);
        default:
            return 0;
    }
}

}  // namespace stud::render_host::vk_wire
