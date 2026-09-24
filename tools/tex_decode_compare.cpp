// Compares Stud's own software ETC/EAC decoder against a GPU that
// implements the same formats in hardware.
//
// This exists because "some textures look broken" cannot be debugged by
// looking at Roblox: the decoder handles several formats, several block
// modes and several sizes, and the eye cannot say which combination is
// wrong. A machine with an Intel iGPU has real ETC2 in hardware, so it can
// decode the same bytes and be believed.
//
// It found the answer in one run: every ETC2 colour format matches
// hardware byte for byte, including ETC1, which has no Vulkan format of
// its own because it is a strict subset of ETC2_RGB, and whose legacy
// block modes random blocks exercise. So the decoder was never the bug,
// and the corruption was in the plumbing around it (stale handle
// bookkeeping and unaligned buffer offsets; see vulkan_client.cpp).
//
// EAC R11/RG11 differ from hardware in the low bit of ~9% of 16-bit
// values: a rounding difference in the 11-to-16-bit expansion, far below
// anything visible, and left alone rather than tuned to match one vendor.
//
// Build: it is a normal target in this directory. Run it against the
// device that has hardware ETC2, which on a hybrid laptop is not the
// default one:
//
//   VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json \
//       ./build/tools/stud_tex_decode_compare
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <random>
#include <vector>
#include "texture_decode.h"

#define VKCHECK(x) do { VkResult r_=(x); if(r_!=VK_SUCCESS){printf("FAIL %s = %d\n",#x,r_);exit(1);} } while(0)

VkInstance inst; VkPhysicalDevice phys; VkDevice dev; VkQueue queue; uint32_t qfam;
VkCommandPool pool;

uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties p; vkGetPhysicalDeviceMemoryProperties(phys,&p);
    for(uint32_t i=0;i<p.memoryTypeCount;i++)
        if((bits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&want)==want) return i;
    printf("no memory type\n"); exit(1);
}

struct Buf { VkBuffer b; VkDeviceMemory m; void* p; };
Buf mkbuf(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buf o{}; VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size=size; ci.usage=usage; VKCHECK(vkCreateBuffer(dev,&ci,0,&o.b));
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(dev,o.b,&rq);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize=rq.size; ai.memoryTypeIndex=mem_type(rq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKCHECK(vkAllocateMemory(dev,&ai,0,&o.m)); VKCHECK(vkBindBufferMemory(dev,o.b,o.m,0));
    VKCHECK(vkMapMemory(dev,o.m,0,rq.size,0,&o.p)); return o;
}
struct Img { VkImage i; VkDeviceMemory m; };
Img mkimg(VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    Img o{}; VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType=VK_IMAGE_TYPE_2D; ci.format=f; ci.extent={w,h,1}; ci.mipLevels=1;
    ci.arrayLayers=1; ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL;
    ci.usage=usage; ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(dev,&ci,0,&o.i));
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(dev,o.i,&rq);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize=rq.size; ai.memoryTypeIndex=mem_type(rq.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKCHECK(vkAllocateMemory(dev,&ai,0,&o.m)); VKCHECK(vkBindImageMemory(dev,o.i,o.m,0)); return o;
}
VkCommandBuffer begin() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool=pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1;
    VkCommandBuffer cb; VKCHECK(vkAllocateCommandBuffers(dev,&ai,&cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; VKCHECK(vkBeginCommandBuffer(cb,&bi)); return cb;
}
void end(VkCommandBuffer cb) {
    VKCHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&cb;
    VKCHECK(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE)); VKCHECK(vkQueueWaitIdle(queue));
}
void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout=from; b.newLayout=to; b.image=img;
    b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,0,0,0,1,&b);
}

int test(VkFormat fmt, const char* name, uint32_t w, uint32_t h) {
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(phys,fmt,&fp);
    if(!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT)) { printf("  %-26s %4ux%-4u SKIP (no hardware blit)\n",name,w,h); return 0; }

    const uint64_t enc = stud::texture_decode::encoded_size(fmt,w,h);
    const uint64_t dec = stud::texture_decode::decoded_size(fmt,w,h);
    const VkFormat sub = stud::texture_decode::substitute(fmt);

    std::vector<uint8_t> src(enc);
    std::mt19937 rng(12345 + w*7 + h*13 + (uint32_t)fmt);
    for(auto& b : src) b = (uint8_t)(rng()&0xFF);

    Buf up = mkbuf(enc, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    memcpy(up.p, src.data(), enc);
    Img ci = mkimg(fmt,w,h,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    Img ri = mkimg(sub,w,h,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    Buf down = mkbuf(dec, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    VkCommandBuffer cb = begin();
    barrier(cb,ci.i,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{}; c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; c.imageExtent={w,h,1};
    vkCmdCopyBufferToImage(cb,up.b,ci.i,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);
    barrier(cb,ci.i,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cb,ri.i,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit bl{}; bl.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; bl.dstSubresource=bl.srcSubresource;
    bl.srcOffsets[1]={(int)w,(int)h,1}; bl.dstOffsets[1]={(int)w,(int)h,1};
    vkCmdBlitImage(cb,ci.i,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,ri.i,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&bl,VK_FILTER_NEAREST);
    barrier(cb,ri.i,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImageToBuffer(cb,ri.i,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,down.b,1,&c);
    end(cb);

    std::vector<uint8_t> mine(dec,0);
    if(!stud::texture_decode::decode(fmt,src.data(),w,h,mine.data())) { printf("  %-26s decode() refused\n",name); return 1; }

    const uint8_t* hw = (const uint8_t*)down.p;
    uint64_t diff=0, worst=0;
    for(uint64_t i=0;i<dec;i++){ int d = abs((int)hw[i]-(int)mine[i]); if(d){++diff; if((uint64_t)d>worst) worst=d;} }
    printf("  %-26s %4ux%-4u  bytes=%-8llu differing=%-8llu (%.1f%%) worst=%llu %s\n",
           name,w,h,(unsigned long long)dec,(unsigned long long)diff,100.0*diff/dec,
           (unsigned long long)worst, diff==0?"OK":"MISMATCH");
    return diff!=0;
}

int main() {
    VkApplicationInfo ap{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ap.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&ap;
    VKCHECK(vkCreateInstance(&ici,0,&inst));
    uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,0); std::vector<VkPhysicalDevice> d(n);
    vkEnumeratePhysicalDevices(inst,&n,d.data());
    phys=VK_NULL_HANDLE;
    for(auto pd : d){ VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(pd,&f);
        if(f.textureCompressionETC2){ phys=pd; break; } }
    if(!phys){ printf("no device with hardware ETC2, cannot establish ground truth\n"); return 2; }
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(phys,&pp);
    printf("ground truth device: %s\n", pp.deviceName);
    uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(phys,&qn,0);
    std::vector<VkQueueFamilyProperties> qs(qn); vkGetPhysicalDeviceQueueFamilyProperties(phys,&qn,qs.data());
    qfam=0; for(uint32_t i=0;i<qn;i++) if(qs[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){qfam=i;break;}
    float prio=1.0f; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex=qfam; qi.queueCount=1; qi.pQueuePriorities=&prio;
    VkPhysicalDeviceFeatures want{}; want.textureCompressionETC2=VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount=1;
    dci.pQueueCreateInfos=&qi; dci.pEnabledFeatures=&want;
    VKCHECK(vkCreateDevice(phys,&dci,0,&dev)); vkGetDeviceQueue(dev,qfam,0,&queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex=qfam;
    VKCHECK(vkCreateCommandPool(dev,&pci,0,&pool));

    struct { VkFormat f; const char* n; } fmts[] = {
        {VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,"ETC2_RGB_UNORM"},
        {VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK,"ETC2_RGB_SRGB"},
        {VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK,"ETC2_RGBA1_UNORM"},
        {VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,"ETC2_RGBA8_UNORM"},
        {VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK,"ETC2_RGBA8_SRGB"},
        {VK_FORMAT_EAC_R11_UNORM_BLOCK,"EAC_R11_UNORM"},
        {VK_FORMAT_EAC_R11G11_UNORM_BLOCK,"EAC_RG11_UNORM"},
    };
    int bad=0;
    for(auto& f : fmts){ bad += test(f.f,f.n,64,64); bad += test(f.f,f.n,60,33); bad += test(f.f,f.n,4,4); }
    printf("\n%s\n", bad? "DECODER DISAGREES WITH HARDWARE" : "decoder matches hardware everywhere");
    return bad?1:0;
}
