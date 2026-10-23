// Vulkan contracts shared by replay and the asset-independent regression probe.
#pragma once
#include <vulkan/vulkan.h>
#include <stdint.h>

static inline VkFormat nvn_vk_compressed_format(int f)
{
    static const VkFormat bc[] = {
        VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
        VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK,
        VK_FORMAT_BC1_RGB_SRGB_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
        VK_FORMAT_BC2_SRGB_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK,
        VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_SNORM_BLOCK,
        VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_SNORM_BLOCK,
        VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK,
        VK_FORMAT_BC6H_SFLOAT_BLOCK, VK_FORMAT_BC6H_UFLOAT_BLOCK
    };
    if (f >= 0x41 && f <= 0x50) return bc[f - 0x41];
    if (f >= 0x79 && f <= 0x94)
        return (VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK +
                          2 * ((f - 0x79) % 14) + (f >= 0x87));
    return VK_FORMAT_UNDEFINED;
}

static inline VkFormat nvn_vk_bc_fallback(int f)
{
    switch (f) {
    case 0x41: case 0x42: case 0x43: case 0x44: case 0x4d: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x45: case 0x46: case 0x47: case 0x48: case 0x4e: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x49: return VK_FORMAT_R8_UNORM;
    case 0x4a: return VK_FORMAT_R8_SNORM;
    case 0x4b: return VK_FORMAT_R8G8_UNORM;
    case 0x4c: return VK_FORMAT_R8G8_SNORM;
    case 0x4f: case 0x50: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static inline int nvn_vk_compressed_supported(VkFormat f, VkFormatFeatureFlags optimal,
                                             const VkPhysicalDeviceFeatures *enabled)
{
    if (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK) {
        if (!enabled->textureCompressionBC) return 0;
    } else if (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) {
        if (!enabled->textureCompressionASTC_LDR) return 0;
    } else return 0;
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (optimal & need) == need;
}

static inline VkImageUsageFlags nvn_vk_image_usage(VkFormatFeatureFlags optimal, int depth)
{
    // Sampling support does not imply renderability (BC/ASTC and RGBA8_SNORM).
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((optimal & need) != need) return 0;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (depth) {
        if (!(optimal & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) return 0;
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else if (optimal & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    return usage;
}

static inline VkImageAspectFlags nvn_vk_aspect(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

static inline int nvn_vk_image_shape(int target, uint32_t w, uint32_t h, uint32_t d,
                                     VkImageCreateInfo *ii, VkImageViewType *view)
{
    ii->imageType = VK_IMAGE_TYPE_2D;
    ii->extent = (VkExtent3D){ w ? w : 1, h ? h : 1, 1 };
    ii->arrayLayers = 1;
    ii->flags = 0;
    *view = VK_IMAGE_VIEW_TYPE_2D;
    switch (target) {
    case 0: case 3: // 1D / 1D array: NVN stores the layer count in height.
        ii->imageType = VK_IMAGE_TYPE_1D; ii->extent.height = 1;
        ii->arrayLayers = target == 3 && h ? h : 1;
        *view = target == 3 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        break;
    case 1: case 7: break; // 2D / rectangle
    case 2:
        ii->imageType = VK_IMAGE_TYPE_3D; ii->extent.depth = d ? d : 1;
        *view = VK_IMAGE_VIEW_TYPE_3D;
        break;
    case 4:
        ii->arrayLayers = d ? d : 1; *view = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        break;
    case 8: case 9:
        if (ii->extent.width != ii->extent.height || d > UINT32_MAX / 6) return 0;
        ii->arrayLayers = target == 8 ? 6 : (d ? d : 1) * 6;
        ii->flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        *view = target == 8 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        break;
    default: return 0; // Buffer and multisample textures need separate upload paths.
    }
    return 1;
}

static inline VkComponentMapping nvn_vk_components(int f, const uint8_t swz[4], int set)
{
    VkComponentSwizzle base[6] = { VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE,
        VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A };
    // Compose format defaults with the guest swizzle, so RGBX's unused byte
    // cannot leak even when the guest routes A into a different channel.
    int ch = f >= 0x01 && f <= 0x30 ? 1 + (f - 1) / 12 : 4;
    if (f == 0x37 || f == 0x3a || f == 0x3c || f == 0x3f || f == 0x40 ||
        f == 0x41 || f == 0x45 || (f >= 0x6d && f <= 0x78) ||
        f == 0x95 || f == 0x96 || f == 0x99 || f == 0x9b) ch = 3;
    if (f == 0x49 || f == 0x4a) ch = 1;
    if (f == 0x4b || f == 0x4c) ch = 2;
    if (ch < 4) base[5] = VK_COMPONENT_SWIZZLE_ONE;
    if (ch < 3) base[4] = VK_COMPONENT_SWIZZLE_ZERO;
    if (ch < 2) base[3] = VK_COMPONENT_SWIZZLE_ZERO;
    for (int i = 0; i < 4 && set; i++) if (swz[i] > 5) set = 0;
    VkComponentMapping out = { base[set ? swz[0] : 2], base[set ? swz[1] : 3],
                               base[set ? swz[2] : 4], base[set ? swz[3] : 5] };
    return out;
}

static inline void nvn_vk_pass_dependencies(VkSubpassDependency dep[2])
{
    // The implicit exit dependency has dstAccessMask=0: a layout transition
    // alone does not make render-target writes visible to texture sampling.
    const VkPipelineStageFlags attachment = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags reads = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    const VkAccessFlags writes = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[0] = (VkSubpassDependency){ VK_SUBPASS_EXTERNAL, 0,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, attachment,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, reads | writes, 0 };
    dep[1] = (VkSubpassDependency){ 0, VK_SUBPASS_EXTERNAL,
        attachment, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        writes, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 };
}

static inline int nvn_vk_copy_fits(uint32_t w, uint32_t h, uint32_t d,
                                  int x, int y, int z, int cw, int ch, int cd, int bw, int bh)
{
    if (x < 0 || y < 0 || z < 0 || cw <= 0 || ch <= 0 || cd <= 0 || bw <= 0 || bh <= 0) return 0;
    if ((uint64_t)x + cw > w || (uint64_t)y + ch > h || (uint64_t)z + cd > d) return 0;
    if (x % bw || y % bh) return 0;
    // A compressed extent may be smaller than a block only at the mip edge.
    return (cw % bw == 0 || (uint64_t)x + cw == w) &&
           (ch % bh == 0 || (uint64_t)y + ch == h);
}

// Return zero for a row pitch that would overlap rows or overflow Vulkan's texel count.
static inline uint64_t nvn_vk_copy_pitch(int64_t stride, uint32_t width, uint32_t bw, uint32_t bpb)
{
    if (stride < 0 || !width || !bw || !bpb) return 0;
    uint64_t minimum = ((uint64_t)width + bw - 1) / bw * bpb;
    uint64_t row = stride ? (uint64_t)stride : minimum;
    if (row < minimum || row % bpb || row / bpb > UINT32_MAX / bw) return 0;
    return row;
}
