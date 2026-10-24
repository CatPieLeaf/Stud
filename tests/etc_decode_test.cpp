// render-host's GPU ETC2/EAC decoder against detex, byte for byte.
//
// The engine's process decodes compressed textures with detex, and
// render-host's GPU decoder (shaders/etc_decode.comp, around gfxstream's
// block decoders) now does the same job in its place. They must agree
// exactly: a decoder that is subtly wrong gives textures that look
// plausible and are quietly corrupt. Random blocks reach every ETC2 mode
// (individual, differential, T, H, planar, punch-through) and every EAC
// table; the source rows are padded and the levels layered, so a pitch or
// offset mistake shows too. Runs on every Vulkan device present, the same
// SPIR-V render-host embeds.

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

extern "C" {
#include "detex.h"
}

namespace {

const uint32_t kSpv[] =
#include "etc_decode_spv.h"
    ;

// shaders/etc_decode.comp's push constants.
struct Push {
    uint32_t codec, blocks_x, blocks_y, src, src_row, src_layer, dst, dst_row, dst_layer;
};

struct Codec {
    const char* name;
    uint32_t block_bytes;
    uint32_t texel_bytes;
    bool (*detex)(const uint8_t*, uint32_t, uint32_t, uint8_t*);
};

const Codec kCodecs[] = {
    {"ETC2 RGB8", 8, 4, detexDecompressBlockETC2},
    {"ETC2 RGB8A1", 8, 4, detexDecompressBlockETC2_PUNCHTHROUGH},
    {"ETC2 RGBA8", 16, 4, detexDecompressBlockETC2_EAC},
    {"EAC R11", 8, 2, detexDecompressBlockEAC_R11},
    {"EAC RG11", 16, 4, detexDecompressBlockEAC_RG11},
};

#define CHECK_VK(x)                                                  \
    do {                                                             \
        if ((x) != VK_SUCCESS) {                                     \
            std::printf("FAIL: %s\n", #x);                           \
            return false;                                            \
        }                                                            \
    } while (0)

bool run_on(VkInstance instance, VkPhysicalDevice pd) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    std::printf("%s\n", props.deviceName);

    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &families, nullptr);
    std::vector<VkQueueFamilyProperties> qf(families);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &families, qf.data());
    uint32_t family = 0;
    while (family < families && (qf[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) ++family;
    if (family == families) return true;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev = VK_NULL_HANDLE;
    CHECK_VK(vkCreateDevice(pd, &dci, nullptr, &dev));
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, family, 0, &queue);

    // One host-visible buffer holds the blocks and the texels.
    const VkDeviceSize size = 8u << 20;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    CHECK_VK(vkCreateBuffer(dev, &bci, nullptr, &buffer));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev, buffer, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && mai.memoryTypeIndex == UINT32_MAX; ++i) {
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
            mai.memoryTypeIndex = i;
        }
    }
    VkDeviceMemory memory = VK_NULL_HANDLE;
    CHECK_VK(vkAllocateMemory(dev, &mai, nullptr, &memory));
    CHECK_VK(vkBindBufferMemory(dev, buffer, memory, 0));
    uint8_t* map = nullptr;
    CHECK_VK(vkMapMemory(dev, memory, 0, size, 0, reinterpret_cast<void**>(&map)));

    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 1;
    dslci.pBindings = &binding;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    CHECK_VK(vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &set_layout));
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    CHECK_VK(vkCreatePipelineLayout(dev, &plci, nullptr, &layout));
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = sizeof(kSpv);
    smci.pCode = kSpv;
    VkShaderModule shader = VK_NULL_HANDLE;
    CHECK_VK(vkCreateShaderModule(dev, &smci, nullptr, &shader));
    VkComputePipelineCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = shader;
    cpci.stage.pName = "main";
    cpci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    CHECK_VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    CHECK_VK(vkCreateDescriptorPool(dev, &dpci, nullptr, &dpool));
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = dpool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    CHECK_VK(vkAllocateDescriptorSets(dev, &dsai, &set));
    VkDescriptorBufferInfo dbi{buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wds{};
    wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet = set;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wds.pBufferInfo = &dbi;
    vkUpdateDescriptorSets(dev, 1, &wds, 0, nullptr);
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK_VK(vkCreateCommandPool(dev, &cpi, nullptr, &pool));
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    CHECK_VK(vkAllocateCommandBuffers(dev, &cbai, &cb));

    bool all_match = true;
    std::mt19937_64 rng(1234);
    for (uint32_t codec = 0; codec < 5; ++codec) {
        const Codec& k = kCodecs[codec];
        const uint32_t bx = 61, by = 37, layers = 3;
        const uint32_t src = 16;
        const uint32_t src_row = bx * k.block_bytes / 4 + 2;  // padded rows of blocks
        const uint32_t src_layer = src_row * (by + 1);         // and a padded layer
        const uint32_t dst_row = bx * 4 * k.texel_bytes / 4;
        const uint32_t dst_layer = dst_row * by * 4;
        const uint32_t dst = src + src_layer * layers + 4;
        std::memset(map, 0xcd, size);
        for (uint64_t i = src * 4ull; i < (src + uint64_t{src_layer} * layers) * 4; ++i) {
            map[i] = static_cast<uint8_t>(rng());
        }
        const Push push{codec, bx, by, src, src_row, src_layer, dst, dst_row, dst_layer};
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        CHECK_VK(vkBeginCommandBuffer(cb, &bi));
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cb, (bx + 7) / 8, (by + 7) / 8, layers);
        CHECK_VK(vkEndCommandBuffer(cb));
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        CHECK_VK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        CHECK_VK(vkQueueWaitIdle(queue));

        uint32_t bad = 0;
        uint8_t ref[64];
        for (uint32_t l = 0; l < layers; ++l) {
            for (uint32_t y = 0; y < by; ++y) {
                for (uint32_t x = 0; x < bx; ++x) {
                    const uint8_t* block =
                        map + 4ull * (src + l * src_layer + y * src_row) + x * k.block_bytes;
                    // The engine-side decoder zeroes a block detex refuses.
                    if (!k.detex(block, DETEX_MODE_MASK_ALL, 0, ref)) std::memset(ref, 0, sizeof(ref));
                    for (uint32_t row = 0; row < 4; ++row) {
                        const uint8_t* got = map + 4ull * (dst + l * dst_layer + (y * 4 + row) * dst_row) +
                                             x * 4 * k.texel_bytes;
                        if (std::memcmp(got, ref + row * 4 * k.texel_bytes, 4 * k.texel_bytes) != 0) {
                            ++bad;
                            break;
                        }
                    }
                }
            }
        }
        std::printf("  %-12s %u of %u blocks differ from detex\n", k.name, bad, bx * by * layers);
        if (bad != 0) all_match = false;
    }

    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyDescriptorPool(dev, dpool, nullptr);
    vkDestroyPipeline(dev, pipeline, nullptr);
    vkDestroyShaderModule(dev, shader, nullptr);
    vkDestroyPipelineLayout(dev, layout, nullptr);
    vkDestroyDescriptorSetLayout(dev, set_layout, nullptr);
    vkDestroyBuffer(dev, buffer, nullptr);
    vkFreeMemory(dev, memory, nullptr);
    vkDestroyDevice(dev, nullptr);
    (void)instance;
    return all_match;
}

}  // namespace

int main() {
    if (sizeof(kSpv) < 32) {
        std::printf("render-host was built without glslc, so there is no GPU decoder to test\n");
        return 77;
    }
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    uint32_t count = 0;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS ||
        vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
        std::printf("no Vulkan device to test on\n");
        return 77;  // ctest's own "skipped" convention
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    bool ok = true;
    for (VkPhysicalDevice pd : devices) ok = run_on(instance, pd) && ok;
    vkDestroyInstance(instance, nullptr);
    std::printf(ok ? "every device decodes exactly as detex does\n" : "FAIL\n");
    return ok ? 0 : 1;
}
