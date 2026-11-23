#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef uint64_t VkImageUsageFlags2KHR;
enum {
   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 1,
   VK_IMAGE_USAGE_SAMPLED_BIT = 2,
   VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT = 4,
   VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT = 8,
   VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 16,
   VK_IMAGE_USAGE_TRANSFER_DST_BIT = 32,
   VK_IMAGE_USAGE_STORAGE_BIT = 64,
   VK_FORMAT_R16G16B16A16_SFLOAT = 97,
   VK_IMAGE_TILING_OPTIMAL = 0,
   VK_IMAGE_TILING_LINEAR = 1,
   VK_IMAGE_TYPE_2D = 1,
   VK_SAMPLE_COUNT_1_BIT = 1,
   VK_SHARING_MODE_EXCLUSIVE = 0,
   FDL_LINEAR_FALLBACK_THRESHOLD = 16,
};
struct vk_image {
   uint32_t format, tiling, image_type, samples, mip_levels, array_layers, sharing_mode;
   struct { uint32_t width, height, depth; } extent;
   uint64_t create_flags, external_handle_types, usage;
   bool wsi_legacy_scanout;
};
static bool enabled;
static bool debug_get_option_tiled_fp16(void) { return enabled; }
#include "fp16_layout_under_test.h"

int main(void)
{
   struct vk_image base = {
      .format = VK_FORMAT_R16G16B16A16_SFLOAT, .image_type = VK_IMAGE_TYPE_2D,
      .samples = VK_SAMPLE_COUNT_1_BIT, .mip_levels = 1, .array_layers = 1,
      .extent = {1080,2160,1},
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
   };
   assert(!rd_image_tiled_fp16_supported(&base));
   enabled = true;
   assert(rd_image_tiled_fp16_supported(&base));
   for (unsigned test = 0; test < 13; test++) {
      struct vk_image image = base;
      switch (test) {
      case 0: image.format = 44; break;
      case 1: image.tiling = VK_IMAGE_TILING_LINEAR; break;
      case 2: image.image_type = 2; break;
      case 3: image.samples = 4; break;
      case 4: image.mip_levels = 2; break;
      case 5: image.array_layers = 2; break;
      case 6: image.extent.width = 15; break;
      case 7: image.sharing_mode = 1; break;
      case 8: image.create_flags = 1; break;
      case 9: image.external_handle_types = 1; break;
      case 10: image.wsi_legacy_scanout = true; break;
      case 11: image.usage = VK_IMAGE_USAGE_SAMPLED_BIT; break;
      case 12: image.usage |= UINT64_C(1) << 40; break;
      }
      assert(!rd_image_tiled_fp16_supported(&image));
   }
   const unsigned excluded[] = {VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_USAGE_STORAGE_BIT};
   for (unsigned i = 0; i < 3; i++) {
      struct vk_image image = base;
      image.usage |= excluded[i];
      assert(!rd_image_tiled_fp16_supported(&image));
   }
   base.extent.width = 1003;
   base.extent.height = 1997;
   assert(rd_image_tiled_fp16_supported(&base));
   base.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
   assert(rd_image_tiled_fp16_supported(&base));
   puts("FP16_LAYOUT PASS: opt-in, internal render targets, odd sizes, and conservative exclusions");
   return 0;
}
