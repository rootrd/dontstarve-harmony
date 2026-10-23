// Headless real-GPU probe. Build with scripts/test_vk_device.py, run through hdc.
#include "nvn_vk_compat.h"
#include "nvn_bcdec.h"
#include "vk_probe_shaders.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "%s: %d at line %d\n", #call, r_, __LINE__); exit(2); } } while (0)
static VkDevice dev;
static VkPhysicalDevice pd;
static VkPhysicalDeviceMemoryProperties mem;
static VkQueue queue;
static VkCommandPool pool;
static VkPhysicalDeviceFeatures enabled;
static unsigned passed;

static VkDeviceMemory allocate(VkMemoryRequirements req, VkMemoryPropertyFlags flags)
{
    for (unsigned i = 0; i < mem.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & flags) == flags) {
            VkMemoryAllocateInfo a = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            a.allocationSize = req.size; a.memoryTypeIndex = i;
            VkDeviceMemory m; VK(vkAllocateMemory(dev, &a, NULL, &m)); return m;
        }
    fprintf(stderr, "No memory type for flags %x\n", flags); exit(2);
}

static VkBuffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory *memory, void **mapping)
{
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size = size; ci.usage = usage;
    VkBuffer b; VK(vkCreateBuffer(dev, &ci, NULL, &b));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, b, &req);
    *memory = allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK(vkBindBufferMemory(dev, b, *memory, 0)); VK(vkMapMemory(dev, *memory, 0, size, 0, mapping)); return b;
}

static void probe(const char *name, VkFormat format, int target, unsigned depth, unsigned mip,
                  const void *pixels, size_t bytes, const float expected[4], VkComponentMapping swizzle)
{
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, format, &fp);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; VkImageViewType view_type;
    unsigned edge = 1u << mip;
    if (!nvn_vk_image_shape(target, edge, edge, depth, &ci, &view_type)) exit(2);
    ci.format = format; ci.mipLevels = mip + 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = nvn_vk_image_usage(fp.optimalTilingFeatures, 0);
    if (!ci.usage) { fprintf(stderr, "%s: unsupported format %d\n", name, format); exit(2); }
    VkImage img; VK(vkCreateImage(dev, &ci, NULL, &img));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(dev, img, &req);
    VkDeviceMemory imem = allocate(req, 0); VK(vkBindImageMemory(dev, img, imem, 0));
    VkImageViewCreateInfo iv = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    iv.image = img; iv.viewType = view_type; iv.format = format; iv.components = swizzle;
    iv.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, ci.arrayLayers};
    VkImageView view; VK(vkCreateImageView(dev, &iv, NULL, &view));
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler; VK(vkCreateSampler(dev, &si, NULL, &sampler));
    VkDeviceMemory umem, rmem; void *up, *result;
    VkBuffer upload = buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &umem, &up);
    VkBuffer output = buffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &rmem, &result);
    memcpy(up, pixels, bytes); memset(result, 0xcc, 16);
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 2; dl.pBindings = bindings;
    VkDescriptorSetLayout layout; VK(vkCreateDescriptorSetLayout(dev, &dl, NULL, &layout));
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1; pl.pSetLayouts = &layout;
    VkPipelineLayout pipe_layout; VK(vkCreatePipelineLayout(dev, &pl, NULL, &pipe_layout));
    VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.pCode = target == 2 ? probe_3d : target == 4 ? probe_array : probe_2d;
    sm.codeSize = target == 2 ? sizeof probe_3d : target == 4 ? sizeof probe_array : sizeof probe_2d;
    VkShaderModule module; VK(vkCreateShaderModule(dev, &sm, NULL, &module));
    VkComputePipelineCreateInfo cp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cp.layout = pipe_layout; cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cp.stage.module = module; cp.stage.pName = "main";
    VkPipeline pipeline; VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, NULL, &pipeline));
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1; dp.poolSizeCount = 2; dp.pPoolSizes = sizes;
    VkDescriptorPool dpool; VK(vkCreateDescriptorPool(dev, &dp, NULL, &dpool));
    VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = dpool; da.descriptorSetCount = 1; da.pSetLayouts = &layout;
    VkDescriptorSet set; VK(vkAllocateDescriptorSets(dev, &da, &set));
    VkDescriptorImageInfo ii = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo bi = {output, 0, 16};
    VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    for (unsigned i = 0; i < 2; i++) { wr[i].dstSet = set; wr[i].dstBinding = i; wr[i].descriptorCount = 1; wr[i].descriptorType = bindings[i].descriptorType; }
    wr[0].pImageInfo = &ii; wr[1].pBufferInfo = &bi; vkUpdateDescriptorSets(dev, 2, wr, 0, NULL);
    VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
    VkCommandBuffer cmd; VK(vkAllocateCommandBuffers(dev, &ca, &cmd));
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; VK(vkBeginCommandBuffer(cmd, &begin));
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.image = img; barrier.subresourceRange = iv.subresourceRange;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VkBufferImageCopy copy = {0}; copy.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, ci.arrayLayers};
    copy.imageExtent = (VkExtent3D){1, 1, target == 2 ? depth : 1};
    vkCmdCopyBufferToImage(cmd, upload, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.oldLayout = barrier.newLayout; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cmd, 1, 1, 1);
    VkMemoryBarrier host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER}; host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    VK(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fc = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; VK(vkCreateFence(dev, &fc, NULL, &fence));
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    VK(vkQueueSubmit(queue, 1, &submit, fence)); VK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull));
    float *got = result;
    for (int i = 0; i < 4; i++) if (!isfinite(got[i]) || fabsf(got[i] - expected[i]) > 0.008f) {
        fprintf(stderr, "FAIL %s channel %d: %.6f expected %.6f\n", name, i, got[i], expected[i]); exit(1);
    }
    printf("PASS %s: %.4f %.4f %.4f %.4f (format %d usage %#x)\n", name, got[0], got[1], got[2], got[3], format, ci.usage); passed++;
    vkDestroyFence(dev, fence, NULL); vkFreeCommandBuffers(dev, pool, 1, &cmd);
    vkDestroyDescriptorPool(dev, dpool, NULL); vkDestroyPipeline(dev, pipeline, NULL); vkDestroyShaderModule(dev, module, NULL);
    vkDestroyPipelineLayout(dev, pipe_layout, NULL); vkDestroyDescriptorSetLayout(dev, layout, NULL);
    vkUnmapMemory(dev, umem); vkUnmapMemory(dev, rmem); vkDestroyBuffer(dev, upload, NULL); vkDestroyBuffer(dev, output, NULL);
    vkFreeMemory(dev, umem, NULL); vkFreeMemory(dev, rmem, NULL); vkDestroySampler(dev, sampler, NULL);
    vkDestroyImageView(dev, view, NULL); vkDestroyImage(dev, img, NULL); vkFreeMemory(dev, imem, NULL);
}

int main(void)
{
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ic = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ic.pApplicationInfo = &app;
    VkInstance instance; VK(vkCreateInstance(&ic, NULL, &instance));
    unsigned count = 1; VK(vkEnumeratePhysicalDevices(instance, &count, &pd));
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props); vkGetPhysicalDeviceMemoryProperties(pd, &mem);
    VkPhysicalDeviceFeatures features; vkGetPhysicalDeviceFeatures(pd, &features);
    enabled.textureCompressionBC = features.textureCompressionBC; enabled.textureCompressionASTC_LDR = features.textureCompressionASTC_LDR;
    printf("GPU %s API %u.%u.%u BC=%u ASTC=%u independentBlend=%u\n", props.deviceName,
        VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion),
        enabled.textureCompressionBC, enabled.textureCompressionASTC_LDR, features.independentBlend);
    unsigned nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp); vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    unsigned family = 0; while (family < nq && !(qp[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) family++;
    free(qp); if (family == nq) return 2;
    float priority = 1; VkDeviceQueueCreateInfo qc = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qc.queueFamilyIndex = family; qc.queueCount = 1; qc.pQueuePriorities = &priority;
    VkDeviceCreateInfo dc = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.queueCreateInfoCount = 1; dc.pQueueCreateInfos = &qc; dc.pEnabledFeatures = &enabled;
    VK(vkCreateDevice(pd, &dc, NULL, &dev)); vkGetDeviceQueue(dev, family, 0, &queue);
    VkCommandPoolCreateInfo pc = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pc.queueFamilyIndex = family;
    VK(vkCreateCommandPool(dev, &pc, NULL, &pool));
    const VkComponentMapping identity = {0}; uint8_t block[16] = {0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t decoded[64] = {0};
    nvn_bc_decode(0x41, block, 8, 1, 1, decoded, 4);
    probe("BC1 RGB opaque selector", VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 0, decoded, 4, (float[]){0,0,0,1}, identity);
    nvn_bc_decode(0x42, block, 8, 1, 1, decoded, 4);
    probe("BC1 RGBA transparent selector", VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 0, decoded, 4, (float[]){0,0,0,0}, identity);
    uint8_t gray[] = {128,128,128,255};
    probe("sRGB mip 1x1", VK_FORMAT_R8G8B8A8_SRGB, 1, 1, 2, gray, 4, (float[]){.215861f,.215861f,.215861f,1}, identity);
    int8_t signed_rg[] = {-127,127};
    probe("BC5 SNORM fallback", VK_FORMAT_R8G8_SNORM, 1, 1, 0, signed_rg, 2, (float[]){-1,1,0,1}, identity);
    int8_t signed_rgba[] = {-127,127,0,127};
    probe("RGBA8 SNORM sampled usage", VK_FORMAT_R8G8B8A8_SNORM, 1, 1, 0, signed_rgba, 4, (float[]){-1,1,0,1}, identity);
    uint8_t layers[80] = {0}; layers[76]=255; layers[78]=255; layers[79]=255;
    probe("array layer 19", VK_FORMAT_R8G8B8A8_UNORM, 4, 20, 0, layers, sizeof layers, (float[]){1,0,1,1}, identity);
    uint8_t volume[] = {0,0,0,0, 0,255,0,255};
    probe("3D slice 1", VK_FORMAT_R8G8B8A8_UNORM, 2, 2, 0, volume, sizeof volume, (float[]){0,1,0,1}, identity);
    uint8_t rgbx[] = {255,0,0,0};
    probe("RGBX alpha one", VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 0, rgbx, 4, (float[]){1,0,0,1}, nvn_vk_components(0x6d, NULL, 0));
    VkFormatProperties astc; vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &astc);
    if (nvn_vk_compressed_supported(VK_FORMAT_ASTC_4x4_UNORM_BLOCK, astc.optimalTilingFeatures, &enabled)) {
        uint8_t solid[] = {0xfc,0xfd,0xff,0xff,0xff,0xff,0xff,0xff, 0xff,0xff,0,0,0,0,0xff,0xff};
        probe("ASTC native mip 1x1", VK_FORMAT_ASTC_4x4_UNORM_BLOCK, 1, 1, 2, solid, 16, (float[]){1,0,0,1}, identity);
    }
    printf("Device texture probe: %u passed\n", passed);
    vkDestroyCommandPool(dev, pool, NULL); vkDestroyDevice(dev, NULL); vkDestroyInstance(instance, NULL); return 0;
}
