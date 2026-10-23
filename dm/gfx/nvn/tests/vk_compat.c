#include "../nvn_vk_compat.h"
#include "../../tex/nvn_bcdec.h"
#include "../../tex/nvn_deswizzle.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void bc_pixels(void)
{
    // BC1 three-colour selector 3: opaque black in RGB, transparent in RGBA.
    uint8_t src[16] = {0, 0, 255, 255, 255, 255, 255, 255};
    uint8_t dst[128];
    for (int f = 0x41; f <= 0x46; f++) {
        if (f == 0x43 || f == 0x44) continue;
        for (unsigned w = 1; w <= 4; w++) for (unsigned h = 1; h <= 4; h++) {
            memset(dst, 0xa5, sizeof dst);
            CHECK(!nvn_bc_decode(f, src, 8, w, h, dst, 24));
            for (unsigned y = 0; y < h; y++) {
                for (unsigned x = 0; x < w; x++) {
                    CHECK(dst[y * 24 + x * 4] == 0);
                    CHECK(dst[y * 24 + x * 4 + 3] == ((f == 0x41 || f == 0x45) ? 255 : 0));
                }
                CHECK(dst[y * 24 + w * 4] == 0xa5);
            }
            CHECK(dst[h * 24] == 0xa5);
        }
    }
    // BC3 endpoint 0 is alpha=128; colour endpoint 0 is pure red.
    memset(src, 0, sizeof src); src[0] = 128; src[9] = 0xf8;
    CHECK(!nvn_bc_decode(0x44, src, 16, 3, 2, dst, 16));
    CHECK(dst[0] == 255 && dst[1] == 0 && dst[2] == 0 && dst[3] == 128);
    // BC4/5 signed endpoints must remain negative in SNORM destinations.
    memset(src, 0, sizeof src); src[0] = 0x80; src[8] = 0x7f;
    CHECK(!nvn_bc_decode(0x4a, src, 8, 1, 1, dst, 8));
    CHECK((int8_t)dst[0] == -127);
    CHECK(!nvn_bc_decode(0x4c, src, 16, 1, 1, dst, 8));
    CHECK((int8_t)dst[0] == -127 && dst[1] == 127);
}

int main(void)
{
    const VkFormatFeatureFlags sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    VkPhysicalDeviceFeatures enabled = {0};
    CHECK(!nvn_vk_compressed_supported(VK_FORMAT_ASTC_4x4_UNORM_BLOCK, sampled, &enabled));
    enabled.textureCompressionASTC_LDR = 1;
    CHECK(nvn_vk_compressed_supported(VK_FORMAT_ASTC_4x4_UNORM_BLOCK, sampled, &enabled));
    CHECK(!nvn_vk_compressed_supported(VK_FORMAT_BC3_UNORM_BLOCK, sampled, &enabled));
    enabled.textureCompressionBC = 1;
    CHECK(nvn_vk_compressed_supported(VK_FORMAT_BC3_UNORM_BLOCK, sampled, &enabled));
    CHECK(!nvn_vk_compressed_supported(VK_FORMAT_BC3_SRGB_BLOCK, 0, &enabled));
    CHECK(!nvn_vk_compressed_supported(VK_FORMAT_BC5_SNORM_BLOCK, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, &enabled));
    CHECK(nvn_vk_compressed_format(0x48) == VK_FORMAT_BC3_SRGB_BLOCK);
    CHECK(nvn_vk_compressed_format(0x94) == VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
    CHECK(nvn_vk_bc_fallback(0x48) == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(nvn_vk_bc_fallback(0x4c) == VK_FORMAT_R8G8_SNORM);
    CHECK(nvn_vk_bc_fallback(0x4f) == VK_FORMAT_R16G16B16A16_SFLOAT);
    CHECK(!(nvn_vk_image_usage(sampled, 0) & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
    CHECK(nvn_vk_image_usage(sampled | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, 0) & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    CHECK(nvn_vk_image_usage(sampled, 1) == 0);
    CHECK(nvn_vk_aspect(VK_FORMAT_D24_UNORM_S8_UINT) == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));

    VkImageCreateInfo ii = {0}; VkImageViewType view;
    CHECK(nvn_vk_image_shape(4, 128, 64, 32, &ii, &view));
    CHECK(ii.extent.depth == 1 && ii.arrayLayers == 32 && ii.flags == 0 && view == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    CHECK(nvn_vk_image_shape(2, 32, 32, 32, &ii, &view));
    CHECK(ii.imageType == VK_IMAGE_TYPE_3D && ii.extent.depth == 32 && ii.arrayLayers == 1 && view == VK_IMAGE_VIEW_TYPE_3D);
    CHECK(nvn_vk_image_shape(9, 64, 64, 2, &ii, &view));
    CHECK(ii.arrayLayers == 12 && ii.extent.depth == 1 && ii.flags == VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT && view == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY);
    CHECK(nvn_vk_image_shape(3, 64, 5, 1, &ii, &view));
    CHECK(ii.extent.height == 1 && ii.arrayLayers == 5 && view == VK_IMAGE_VIEW_TYPE_1D_ARRAY);
    CHECK(!nvn_vk_image_shape(8, 64, 32, 1, &ii, &view));
    CHECK(!nvn_vk_image_shape(10, 64, 1, 1, &ii, &view));

    const uint8_t font[] = {1, 1, 1, 2}, rgbx[] = {5, 2, 3, 4};
    VkComponentMapping sw = nvn_vk_components(0x01, font, 1);
    CHECK(sw.r == VK_COMPONENT_SWIZZLE_ONE && sw.g == sw.r && sw.b == sw.r && sw.a == VK_COMPONENT_SWIZZLE_R);
    sw = nvn_vk_components(0x6d, rgbx, 1);
    CHECK(sw.r == VK_COMPONENT_SWIZZLE_ONE && sw.a == VK_COMPONENT_SWIZZLE_B);
    sw = nvn_vk_components(0x41, NULL, 0);
    CHECK(sw.a == VK_COMPONENT_SWIZZLE_ONE);

    CHECK(nvn_vk_copy_fits(1, 1, 1, 0, 0, 0, 1, 1, 1, 4, 4));
    CHECK(nvn_vk_copy_fits(2, 2, 1, 0, 0, 0, 2, 2, 1, 4, 4));
    CHECK(nvn_vk_copy_fits(7, 5, 32, 4, 4, 0, 3, 1, 32, 4, 4));
    CHECK(!nvn_vk_copy_fits(2, 2, 1, 0, 0, 0, 4, 4, 1, 4, 4));
    CHECK(!nvn_vk_copy_fits(8, 8, 1, 0, 0, 0, 3, 3, 1, 4, 4));
    CHECK(!nvn_vk_copy_fits(8, 8, 1, -1, 0, 0, 8, 8, 1, 1, 1));
    CHECK(!nvn_vk_copy_fits(8, 8, 6, 0, 0, 5, 8, 8, 2, 1, 1));
    CHECK(nvn_vk_copy_pitch(0, 1, 4, 16) == 16);
    CHECK(nvn_vk_copy_pitch(64, 8, 4, 16) == 64);
    CHECK(!nvn_vk_copy_pitch(16, 8, 4, 16));
    CHECK(!nvn_vk_copy_pitch(33, 8, 4, 16));
    CHECK(!nvn_vk_copy_pitch(-1, 8, 4, 16));
    CHECK(!nvn_vk_copy_pitch(INT64_MAX, 8, 4, 16));
    CHECK(nvn_bl_offset(15, 3, 1, 16, 4, 0, 0) + 1 == 608);
    VkSubpassDependency dep[2]; nvn_vk_pass_dependencies(dep);
    CHECK(dep[1].dstSubpass == VK_SUBPASS_EXTERNAL && dep[1].dstAccessMask & VK_ACCESS_MEMORY_READ_BIT);
    CHECK(dep[1].srcStageMask & VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
    CHECK(dep[0].dependencyFlags == 0 && dep[1].dependencyFlags == 0);
    bc_pixels();

    // Hand-placed Tegra markers: 4x4 RGBA8, two z slices, one GOB per slice.
    nvn_texdesc d = {2, 0x25, 0, 0, 4, 4, 2, 1, 0, 0};
    uint8_t tiled[1024] = {0}, linear[128] = {0};
    for (unsigned z = 0; z < 2; z++) for (unsigned y = 0; y < 4; y++) for (unsigned x = 0; x < 16; x++)
        tiled[z * 512 + y / 2 * 64 + y % 2 * 16 + x] = (uint8_t)(z * 64 + y * 16 + x);
    CHECK(!nvn_deswizzle(&d, 0, 0, tiled, linear, 16));
    for (unsigned i = 0; i < 128; i++) CHECK(linear[i] == i);
    printf("Vulkan compatibility: %u checks passed\n", checks);
    return 0;
}
