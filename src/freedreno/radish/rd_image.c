#include "rd_private.h"
#include "util/cache_ops.h"
#include "drm-uapi/drm_fourcc.h"
#include "util/u_debug.h"
#include "vk_format.h"
#include "vk_format_info.h"
#include "vk_util.h"
#include "vk_ycbcr_conversion.h"
#include "util/format/u_format.h"
#include <limits.h>
#include <math.h>

DEBUG_GET_ONCE_BOOL_OPTION(gpu_image_copy, "RADISH_GPU_IMAGE_COPY", true)
DEBUG_GET_ONCE_BOOL_OPTION(image_transfer_cache_ranges, "RADISH_IMAGE_TRANSFER_CACHE_RANGES", true)
DEBUG_GET_ONCE_BOOL_OPTION(image_transfer_cache_debug, "RADISH_IMAGE_TRANSFER_CACHE_DEBUG", false)
DEBUG_GET_ONCE_BOOL_OPTION(cpu_image_clear_bulk, "RADISH_CPU_IMAGE_CLEAR_BULK", true)
DEBUG_GET_ONCE_BOOL_OPTION(cpu_image_clear_debug, "RADISH_CPU_IMAGE_CLEAR_DEBUG", false)
DEBUG_GET_ONCE_BOOL_OPTION(cpu_image_copy_bulk, "RADISH_CPU_IMAGE_COPY_BULK", true)
DEBUG_GET_ONCE_BOOL_OPTION(cpu_image_copy_debug, "RADISH_CPU_IMAGE_COPY_DEBUG", false)

bool
rd_ycbcr_format_supported(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
   case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
   case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
   case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
   case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
      return true;
   default:
      return false;
   }
}

const struct rd_image_level *
rd_image_plane_level(const struct rd_image *image, uint32_t plane, uint32_t level)
{
   assert(plane < vk_format_get_plane_count(image->vk.format));
   assert(level < image->vk.mip_levels);
   return plane ? &image->extra_planes[plane - 1].levels[level] : &image->levels[level];
}

const struct fdl_layout *
rd_image_plane_native(const struct rd_image *image, uint32_t plane)
{
   assert(plane < vk_format_get_plane_count(image->vk.format));
   return plane ? &image->extra_planes[plane - 1].native_layout : &image->native_layout;
}

static uint32_t
rd_image_aspect_plane(VkFormat format, VkImageAspectFlags aspect)
{
   if (!aspect)
      return UINT32_MAX;
   if (!rd_ycbcr_format_supported(format))
      return !(aspect & ~vk_format_aspects(format)) ? 0 : UINT32_MAX;
   if ((aspect & (aspect - 1)) || !(aspect & vk_format_aspects(format)))
      return UINT32_MAX;
   return aspect == VK_IMAGE_ASPECT_PLANE_0_BIT ? 0 :
          aspect == VK_IMAGE_ASPECT_PLANE_1_BIT ? 1 :
          aspect == VK_IMAGE_ASPECT_PLANE_2_BIT ? 2 : UINT32_MAX;
}

static VkExtent3D
rd_image_plane_extent(const struct rd_image *image, uint32_t plane, uint32_t level)
{
   VkExtent3D extent = vk_image_mip_level_extent(&image->vk, level);
   const struct vk_format_ycbcr_info *info = vk_format_get_ycbcr_info(image->vk.format);
   if (info) {
      extent.width = DIV_ROUND_UP(extent.width, info->planes[plane].denominator_scales[0]);
      extent.height = DIV_ROUND_UP(extent.height, info->planes[plane].denominator_scales[1]);
   }
   return extent;
}

static uint32_t
rd_image_texel_size(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8_UNORM:
   case VK_FORMAT_R8_SNORM:
   case VK_FORMAT_R8_UINT:
   case VK_FORMAT_R8_SINT:
   case VK_FORMAT_R8G8_UNORM:
   case VK_FORMAT_R8G8_SNORM:
   case VK_FORMAT_R8G8_UINT:
   case VK_FORMAT_R8G8_SINT:
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SNORM:
   case VK_FORMAT_R8G8B8A8_UINT:
   case VK_FORMAT_R8G8B8A8_SINT:
   case VK_FORMAT_R8G8B8A8_SRGB:
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
   case VK_FORMAT_R16_UNORM:
   case VK_FORMAT_R16_SNORM:
   case VK_FORMAT_R16_UINT:
   case VK_FORMAT_R16_SINT:
   case VK_FORMAT_R16_SFLOAT:
   case VK_FORMAT_R16G16_UNORM:
   case VK_FORMAT_R16G16_SNORM:
   case VK_FORMAT_R16G16_UINT:
   case VK_FORMAT_R16G16_SINT:
   case VK_FORMAT_R16G16_SFLOAT:
   case VK_FORMAT_R16G16B16A16_UNORM:
   case VK_FORMAT_R16G16B16A16_SNORM:
   case VK_FORMAT_R16G16B16A16_UINT:
   case VK_FORMAT_R16G16B16A16_SINT:
   case VK_FORMAT_R16G16B16A16_SFLOAT:
   case VK_FORMAT_R32_UINT:
   case VK_FORMAT_R32_SINT:
   case VK_FORMAT_R32_SFLOAT:
   case VK_FORMAT_R32G32_UINT:
   case VK_FORMAT_R32G32_SINT:
   case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
   case VK_FORMAT_R5G6B5_UNORM_PACK16:
   case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
   case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
   case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
   case VK_FORMAT_A8B8G8R8_UINT_PACK32:
   case VK_FORMAT_A8B8G8R8_SINT_PACK32:
   case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
   case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
   case VK_FORMAT_A2R10G10B10_UINT_PACK32:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
   case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
   case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
   case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
   case VK_FORMAT_B5G6R5_UNORM_PACK16:
    case VK_FORMAT_B8G8R8A8_SNORM:
    case VK_FORMAT_B8G8R8A8_UINT:
    case VK_FORMAT_B8G8R8A8_SINT:
   case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
   case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
   case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
   case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
   case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
   case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
   case VK_FORMAT_EAC_R11_UNORM_BLOCK:
   case VK_FORMAT_EAC_R11_SNORM_BLOCK:
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
    case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
   case VK_FORMAT_D16_UNORM:
   case VK_FORMAT_D32_SFLOAT:
   case VK_FORMAT_S8_UINT:
   case VK_FORMAT_D24_UNORM_S8_UINT:
      return vk_format_get_blocksize(format);
   default:
      return 0;
   }
}

static bool
rd_image_color_attachment_supported(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8_UNORM:
   case VK_FORMAT_R8_SNORM:
   case VK_FORMAT_R8_UINT:
   case VK_FORMAT_R8_SINT:
   case VK_FORMAT_R8G8_UNORM:
   case VK_FORMAT_R8G8_SNORM:
   case VK_FORMAT_R8G8_UINT:
   case VK_FORMAT_R8G8_SINT:
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SNORM:
   case VK_FORMAT_R8G8B8A8_UINT:
   case VK_FORMAT_R8G8B8A8_SINT:
   case VK_FORMAT_R8G8B8A8_SRGB:
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
   case VK_FORMAT_R16_UNORM:
   case VK_FORMAT_R16_SNORM:
   case VK_FORMAT_R16_UINT:
   case VK_FORMAT_R16_SINT:
   case VK_FORMAT_R16_SFLOAT:
   case VK_FORMAT_R16G16_UNORM:
   case VK_FORMAT_R16G16_SNORM:
   case VK_FORMAT_R16G16_UINT:
   case VK_FORMAT_R16G16_SINT:
   case VK_FORMAT_R16G16_SFLOAT:
   case VK_FORMAT_R16G16B16A16_UNORM:
   case VK_FORMAT_R16G16B16A16_SNORM:
   case VK_FORMAT_R16G16B16A16_UINT:
   case VK_FORMAT_R16G16B16A16_SINT:
   case VK_FORMAT_R16G16B16A16_SFLOAT:
   case VK_FORMAT_R32_UINT:
   case VK_FORMAT_R32_SINT:
   case VK_FORMAT_R32_SFLOAT:
   case VK_FORMAT_R32G32_UINT:
   case VK_FORMAT_R32G32_SINT:
   case VK_FORMAT_R32G32_SFLOAT:
   case VK_FORMAT_R32G32B32A32_UINT:
   case VK_FORMAT_R32G32B32A32_SINT:
   case VK_FORMAT_R32G32B32A32_SFLOAT:
   case VK_FORMAT_R5G6B5_UNORM_PACK16:
   case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
   case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
   case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
   case VK_FORMAT_A8B8G8R8_UINT_PACK32:
   case VK_FORMAT_A8B8G8R8_SINT_PACK32:
   case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
   case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
   case VK_FORMAT_A2R10G10B10_UINT_PACK32:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
   case VK_FORMAT_A2B10G10R10_UINT_PACK32:
   case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
      return true;
   default:
      return false;
   }
}

static bool
rd_image_color_blend_supported(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R5G6B5_UNORM_PACK16:
   case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
   case VK_FORMAT_R8_UNORM:
   case VK_FORMAT_R8G8_UNORM:
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SRGB:
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
   case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
   case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
   case VK_FORMAT_R16_SFLOAT:
   case VK_FORMAT_R16G16_SFLOAT:
   case VK_FORMAT_R16G16B16A16_SFLOAT:
      return true;
   default:
      return false;
   }
}

static bool
rd_image_required_blit_src(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
   case VK_FORMAT_R5G6B5_UNORM_PACK16:
   case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
   case VK_FORMAT_R8_UNORM:
   case VK_FORMAT_R8_SNORM:
   case VK_FORMAT_R8_UINT:
   case VK_FORMAT_R8_SINT:
   case VK_FORMAT_R8G8_UNORM:
   case VK_FORMAT_R8G8_SNORM:
   case VK_FORMAT_R8G8_UINT:
   case VK_FORMAT_R8G8_SINT:
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SNORM:
   case VK_FORMAT_R8G8B8A8_UINT:
   case VK_FORMAT_R8G8B8A8_SINT:
   case VK_FORMAT_R8G8B8A8_SRGB:
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
   case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
   case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
   case VK_FORMAT_A8B8G8R8_UINT_PACK32:
   case VK_FORMAT_A8B8G8R8_SINT_PACK32:
   case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
   case VK_FORMAT_A2B10G10R10_UINT_PACK32:
   case VK_FORMAT_R16_UINT:
   case VK_FORMAT_R16_SINT:
   case VK_FORMAT_R16_SFLOAT:
   case VK_FORMAT_R16G16_UINT:
   case VK_FORMAT_R16G16_SINT:
   case VK_FORMAT_R16G16_SFLOAT:
   case VK_FORMAT_R16G16B16A16_UINT:
   case VK_FORMAT_R16G16B16A16_SINT:
   case VK_FORMAT_R16G16B16A16_SFLOAT:
   case VK_FORMAT_R32_UINT:
   case VK_FORMAT_R32_SINT:
   case VK_FORMAT_R32_SFLOAT:
   case VK_FORMAT_R32G32_UINT:
   case VK_FORMAT_R32G32_SINT:
   case VK_FORMAT_R32G32_SFLOAT:
   case VK_FORMAT_R32G32B32A32_UINT:
   case VK_FORMAT_R32G32B32A32_SINT:
   case VK_FORMAT_R32G32B32A32_SFLOAT:
   case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
   case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
   case VK_FORMAT_D16_UNORM:
   case VK_FORMAT_D32_SFLOAT:
      return true;
   default:
      return false;
   }
}

static bool
rd_image_required_blit_dst(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R5G6B5_UNORM_PACK16:
   case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
   case VK_FORMAT_R8_UNORM:
   case VK_FORMAT_R8_UINT:
   case VK_FORMAT_R8_SINT:
   case VK_FORMAT_R8G8_UNORM:
   case VK_FORMAT_R8G8_UINT:
   case VK_FORMAT_R8G8_SINT:
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_UINT:
   case VK_FORMAT_R8G8B8A8_SINT:
   case VK_FORMAT_R8G8B8A8_SRGB:
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
   case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
   case VK_FORMAT_A8B8G8R8_UINT_PACK32:
   case VK_FORMAT_A8B8G8R8_SINT_PACK32:
   case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
   case VK_FORMAT_A2B10G10R10_UINT_PACK32:
   case VK_FORMAT_R16_UINT:
   case VK_FORMAT_R16_SINT:
   case VK_FORMAT_R16_SFLOAT:
   case VK_FORMAT_R16G16_UINT:
   case VK_FORMAT_R16G16_SINT:
   case VK_FORMAT_R16G16_SFLOAT:
   case VK_FORMAT_R16G16B16A16_UINT:
   case VK_FORMAT_R16G16B16A16_SINT:
   case VK_FORMAT_R16G16B16A16_SFLOAT:
   case VK_FORMAT_R32_UINT:
   case VK_FORMAT_R32_SINT:
   case VK_FORMAT_R32_SFLOAT:
   case VK_FORMAT_R32G32_UINT:
   case VK_FORMAT_R32G32_SINT:
   case VK_FORMAT_R32G32_SFLOAT:
   case VK_FORMAT_R32G32B32A32_UINT:
   case VK_FORMAT_R32G32B32A32_SINT:
   case VK_FORMAT_R32G32B32A32_SFLOAT:
      return true;
   default:
      return false;
   }
}

static bool
rd_image_required_vertex_buffer(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8_UNORM:
   case VK_FORMAT_R8_SNORM:
   case VK_FORMAT_R8_UINT:
   case VK_FORMAT_R8_SINT:
   case VK_FORMAT_R8G8_UNORM:
   case VK_FORMAT_R8G8_SNORM:
   case VK_FORMAT_R8G8_UINT:
   case VK_FORMAT_R8G8_SINT:
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SNORM:
   case VK_FORMAT_R8G8B8A8_UINT:
   case VK_FORMAT_R8G8B8A8_SINT:
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
   case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
   case VK_FORMAT_A8B8G8R8_UINT_PACK32:
   case VK_FORMAT_A8B8G8R8_SINT_PACK32:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
   case VK_FORMAT_R16_UNORM:
   case VK_FORMAT_R16_SNORM:
   case VK_FORMAT_R16_UINT:
   case VK_FORMAT_R16_SINT:
   case VK_FORMAT_R16_SFLOAT:
   case VK_FORMAT_R16G16_UNORM:
   case VK_FORMAT_R16G16_SNORM:
   case VK_FORMAT_R16G16_UINT:
   case VK_FORMAT_R16G16_SINT:
   case VK_FORMAT_R16G16_SFLOAT:
   case VK_FORMAT_R16G16B16A16_UNORM:
   case VK_FORMAT_R16G16B16A16_SNORM:
   case VK_FORMAT_R16G16B16A16_UINT:
   case VK_FORMAT_R16G16B16A16_SINT:
   case VK_FORMAT_R16G16B16A16_SFLOAT:
   case VK_FORMAT_R32_UINT:
   case VK_FORMAT_R32_SINT:
   case VK_FORMAT_R32_SFLOAT:
   case VK_FORMAT_R32G32_UINT:
   case VK_FORMAT_R32G32_SINT:
   case VK_FORMAT_R32G32_SFLOAT:
   case VK_FORMAT_R32G32B32_UINT:
   case VK_FORMAT_R32G32B32_SINT:
   case VK_FORMAT_R32G32B32_SFLOAT:
   case VK_FORMAT_R32G32B32A32_UINT:
   case VK_FORMAT_R32G32B32A32_SINT:
   case VK_FORMAT_R32G32B32A32_SFLOAT:
      return true;
   default:
      return false;
   }
}

static VkImageUsageFlags2KHR
rd_image_shader_usage(VkFormat format)
{
   if (!rd_image_texel_size(format) || !rd_shader_format_supported(format))
      return 0;
   VkImageUsageFlags2KHR usage = VK_IMAGE_USAGE_SAMPLED_BIT;
   if (rd_image_color_attachment_supported(format))
      usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
   if (format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT ||
       format == VK_FORMAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT)
      usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
   if (rd_storage_shader_format_supported(format))
      usage |= VK_IMAGE_USAGE_STORAGE_BIT;
   return usage;
}

static bool
rd_image_parameters(VkFormat format, VkImageType type, VkImageTiling tiling,
                    VkImageUsageFlags2KHR usage, VkImageCreateFlags2KHR flags)
{
   if (rd_ycbcr_format_supported(format))
      return type == VK_IMAGE_TYPE_2D &&
         (tiling == VK_IMAGE_TILING_LINEAR || tiling == VK_IMAGE_TILING_OPTIMAL ||
          tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) && usage &&
         !(usage & ~(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT)) &&
         !(flags & ~(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_ALIAS_BIT));
   if (!rd_image_texel_size(format))
      return false;
   if (vk_format_is_depth_or_stencil(format) && type != VK_IMAGE_TYPE_2D)
      return false;
   if (vk_format_is_compressed(format) &&
       (type != VK_IMAGE_TYPE_2D || tiling != VK_IMAGE_TILING_OPTIMAL))
      return false;
   VkImageUsageFlags2KHR shader_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
   if ((usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) &&
         tiling != VK_IMAGE_TILING_OPTIMAL &&
         tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT)
      return false;
   VkImageUsageFlags2KHR supported = rd_image_shader_usage(format);
   if ((flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) &&
       (flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) &&
       !vk_format_is_depth_or_stencil(format)) {
      const struct vk_format_class_info *class = vk_format_get_class_info(format);
      for (uint32_t i = 0; i < class->format_count; i++) {
         if (!vk_format_is_depth_or_stencil(class->formats[i]))
            supported |= rd_image_shader_usage(class->formats[i]);
      }
      if ((flags & VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT) && vk_format_is_compressed(format)) {
         VkFormat uncompressed = vk_format_get_blocksize(format) == 8 ?
            VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32B32A32_UINT;
         const struct vk_format_class_info *block = vk_format_get_class_info(uncompressed);
         for (uint32_t i = 0; i < block->format_count; i++)
            supported |= rd_image_shader_usage(block->formats[i]);
      }
   }
   if (usage & shader_usage & ~supported)
      return false;
   if (usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) {
      VkImageUsageFlags2KHR attachments = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
      if (!(usage & attachments) ||
          (usage & ~(attachments | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)))
         return false;
   }
   return rd_image_texel_size(format) &&
          (type == VK_IMAGE_TYPE_1D || type == VK_IMAGE_TYPE_2D || type == VK_IMAGE_TYPE_3D) &&
          (tiling == VK_IMAGE_TILING_LINEAR || tiling == VK_IMAGE_TILING_OPTIMAL ||
           tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) &&
          usage && !(usage & ~(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | shader_usage)) &&
          !(flags & ~(VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                       VK_IMAGE_CREATE_ALIAS_BIT | VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT |
                       VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT |
                       VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT)) &&
          (!(flags & VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT) ||
           (type == VK_IMAGE_TYPE_2D && tiling == VK_IMAGE_TILING_OPTIMAL)) &&
          (!(flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) || type == VK_IMAGE_TYPE_2D) &&
          (!(flags & VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT) ||
           (vk_format_is_compressed(format) && (flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT))) &&
          (!(flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT) || type == VK_IMAGE_TYPE_3D);
}

static VkSampleCountFlags
rd_image_sample_counts(VkFormat format, VkImageType type, VkImageTiling tiling,
                       VkImageUsageFlags2KHR usage, VkImageCreateFlags2KHR flags)
{
   VkSampleCountFlags samples = VK_SAMPLE_COUNT_1_BIT;
   if ((rd_image_shader_usage(format) & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) &&
       !vk_format_is_compressed(format) && type == VK_IMAGE_TYPE_2D && tiling == VK_IMAGE_TILING_OPTIMAL &&
       !(usage & VK_IMAGE_USAGE_STORAGE_BIT) &&
       !(flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT))
      samples |= VK_SAMPLE_COUNT_4_BIT;
   return samples;
}

static bool
rd_drm_modifier_format_supported(VkFormat format)
{
   if (rd_ycbcr_format_supported(format))
      return true;
   switch (format) {
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SRGB:
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
      return true;
   default:
      return false;
   }
}

VKAPI_ATTR void VKAPI_CALL
rd_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice handle, VkFormat format,
                                      VkFormatProperties2 *out)
{
   bool ycbcr = rd_ycbcr_format_supported(format);
   VkFormatFeatureFlags flags = rd_image_texel_size(format) ?
      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT : 0;
   VkFormatFeatureFlags buffer = rd_image_required_vertex_buffer(format) ?
      VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT : 0;
   if (rd_shader_format_supported(format)) {
      if (rd_image_texel_size(format))
         flags |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
      if (rd_image_texel_size(format) && rd_shader_format_filterable(format)) {
         flags |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
         if (vk_format_is_compressed(format))
            flags |= VK_FORMAT_FEATURE_BLIT_SRC_BIT;
      }
      if (!vk_format_is_compressed(format) && !vk_format_is_srgb(format) && !vk_format_is_depth_or_stencil(format))
         buffer |= VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
      if (rd_storage_shader_format_supported(format)) {
         flags |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
         buffer |= VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
      }
      if (format == VK_FORMAT_R32_UINT || format == VK_FORMAT_R32_SINT) {
         flags |= VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
         buffer |= VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT;
      }
   }
   VkImageUsageFlags2KHR usage = rd_image_shader_usage(format);
   VkFormatFeatureFlags attachment = 0;
   if (rd_image_required_blit_src(format))
      attachment |= VK_FORMAT_FEATURE_BLIT_SRC_BIT;
   if (rd_image_required_blit_dst(format))
      attachment |= VK_FORMAT_FEATURE_BLIT_DST_BIT;
   VkFormatFeatureFlags linear_blit = rd_image_required_blit_dst(format) ?
      VK_FORMAT_FEATURE_BLIT_DST_BIT : 0;
   if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
      attachment |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
      if (rd_image_color_blend_supported(format))
         attachment |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
   }
   if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
      attachment |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
   if (ycbcr) {
      flags = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
         VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT | VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT |
         VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT;
      buffer = attachment = linear_blit = 0;
   }
   out->formatProperties = (VkFormatProperties) {
      .linearTilingFeatures = vk_format_is_compressed(format) ? 0 : flags | linear_blit,
      .optimalTilingFeatures = flags | attachment,
      .bufferFeatures = buffer,
   };
   VkFormatFeatureFlags2 flags2 = flags;
   if (vk_format_has_depth(format) && (flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
      flags2 |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT;
   vk_foreach_struct(sType, next, out->pNext) {
      if (sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
         VkFormatProperties3 *properties = (VkFormatProperties3 *)next;
         properties->linearTilingFeatures = vk_format_is_compressed(format) ? 0 : flags2 | linear_blit;
         properties->optimalTilingFeatures = flags2 | attachment;
         properties->bufferFeatures = buffer;
      }
   }
   VkFormatFeatureFlags2 modifier_features = flags2 | attachment;
   VkDrmFormatModifierPropertiesListEXT *modifier_list =
      vk_find_struct(out->pNext, DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT);
   if (modifier_list) {
      VK_OUTARRAY_MAKE_TYPED(VkDrmFormatModifierPropertiesEXT, modifier_out,
                             modifier_list->pDrmFormatModifierProperties,
                             &modifier_list->drmFormatModifierCount);
      if (rd_drm_modifier_format_supported(format)) {
         vk_outarray_append_typed(VkDrmFormatModifierPropertiesEXT, &modifier_out, mod) {
            mod->drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
            mod->drmFormatModifierPlaneCount = vk_format_get_plane_count(format);
            mod->drmFormatModifierTilingFeatures = modifier_features;
         }
      }
   }
   VkDrmFormatModifierPropertiesList2EXT *modifier_list2 =
      vk_find_struct(out->pNext, DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT);
   if (modifier_list2) {
      VK_OUTARRAY_MAKE_TYPED(VkDrmFormatModifierProperties2EXT, modifier_out,
                             modifier_list2->pDrmFormatModifierProperties,
                             &modifier_list2->drmFormatModifierCount);
      if (rd_drm_modifier_format_supported(format)) {
         vk_outarray_append_typed(VkDrmFormatModifierProperties2EXT, &modifier_out, mod) {
            mod->drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
            mod->drmFormatModifierPlaneCount = vk_format_get_plane_count(format);
            mod->drmFormatModifierTilingFeatures = modifier_features;
         }
      }
   }
}

static uint32_t
rd_image_max_side(VkImageType type, VkImageCreateFlags2KHR flags)
{
   if (type == VK_IMAGE_TYPE_3D)
      return 256;
   if (type == VK_IMAGE_TYPE_1D || (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT))
      return 4096;
   return RD_IMAGE_MAX_2D_SIZE;
}

VKAPI_ATTR VkResult VKAPI_CALL
rd_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice handle,
   const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *out)
{
   out->imageFormatProperties = (VkImageFormatProperties){0};
   const VkPhysicalDeviceExternalImageFormatInfo *external =
      vk_find_struct_const(info->pNext, PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
   const VkPhysicalDeviceImageDrmFormatModifierInfoEXT *modifier_info =
      vk_find_struct_const(info->pNext, PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT);
   if (info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT &&
       (!modifier_info || modifier_info->drmFormatModifier != DRM_FORMAT_MOD_LINEAR ||
        !rd_drm_modifier_format_supported(info->format)))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   const VkExternalMemoryHandleTypeFlags external_handles =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   if (!rd_image_parameters(info->format, info->type, info->tiling, info->usage, info->flags) ||
       (external && external->handleType &&
        (info->tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT ||
         (external->handleType & ~external_handles))))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   uint32_t side = rd_image_max_side(info->type, info->flags);
   out->imageFormatProperties = (VkImageFormatProperties) {
      .maxExtent = {side, info->type == VK_IMAGE_TYPE_1D ? 1 : side,
                    info->type == VK_IMAGE_TYPE_3D ? side : 1},
      .maxMipLevels = util_logbase2(side) + 1,
      .maxArrayLayers = info->type == VK_IMAGE_TYPE_3D ? 1 : 256,
      .sampleCounts = rd_image_sample_counts(info->format, info->type, info->tiling, info->usage, info->flags),
      .maxResourceSize = RD_IMAGE_MAX_SIZE,
   };
   if (info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
      out->imageFormatProperties.maxExtent = (VkExtent3D){side, side, 1};
      out->imageFormatProperties.maxMipLevels = 1;
      out->imageFormatProperties.maxArrayLayers = 1;
      out->imageFormatProperties.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
   }
   if (info->flags & VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT) {
      out->imageFormatProperties.maxMipLevels = 1;
      out->imageFormatProperties.maxArrayLayers = 1;
   }
   if (rd_ycbcr_format_supported(info->format)) {
      out->imageFormatProperties.maxMipLevels = 1;
      out->imageFormatProperties.maxArrayLayers = 1;
   }
   VkExternalMemoryProperties external_memory = {0};
   if (external && external->handleType) {
      external_memory = (VkExternalMemoryProperties) {
         .externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                                   VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
         .exportFromImportedHandleTypes = external_handles,
         .compatibleHandleTypes = external_handles,
      };
   }
   vk_foreach_struct(sType, next, out->pNext) {
      if (sType == VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES)
         ((VkExternalImageFormatProperties *)next)->externalMemoryProperties =
            external_memory;
      else if (sType == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES)
         ((VkSamplerYcbcrConversionImageFormatProperties *)next)->combinedImageSamplerDescriptorCount =
            vk_format_get_plane_count(info->format);
   }
   return VK_SUCCESS;
}

static bool
rd_image_layout(struct rd_image *image)
{
   VkDeviceSize size = 0;
   if (rd_ycbcr_format_supported(image->vk.format)) {
      const struct vk_format_ycbcr_info *info = vk_format_get_ycbcr_info(image->vk.format);
      for (uint32_t plane = 0; plane < info->n_planes; plane++) {
         struct fdl_layout *native = plane ? &image->extra_planes[plane - 1].native_layout : &image->native_layout;
         struct rd_image_level *level = plane ? &image->extra_planes[plane - 1].levels[0] : &image->levels[0];
         VkExtent3D extent = rd_image_plane_extent(image, plane, 0);
         const struct fdl_image_params parameters = {
            .format = vk_format_to_pipe_format(info->planes[plane].format), .nr_samples = 1,
            .width0 = extent.width, .height0 = extent.height, .depth0 = 1,
            .mip_levels = 1, .array_size = 1,
         };
         fdl5_layout_image(native, &parameters);
         native->mip_levels = 1;
         size = align64(size, 4096);
         if (!native->size || size > RD_IMAGE_MAX_SIZE || native->size > RD_IMAGE_MAX_SIZE - size)
            return false;
         *level = (struct rd_image_level) {
            .offset = size, .row_pitch = fdl_pitch(native, 0), .depth_pitch = native->slices[0].size0,
            .array_pitch = native->layer_size, .size = native->size,
         };
         size += native->size;
      }
      image->size = size;
      return true;
   }
   bool shader = image->vk.usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
   /* Blits and resolves sample the source and render the destination, so
    * optimal transfer-only images of any sampleable format (compressed
    * blit sources included) get the native layout too. */
   if (image->vk.tiling == VK_IMAGE_TILING_OPTIMAL &&
       (image->vk.usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) &&
       (rd_shader_format_supported(image->vk.format) || image->vk.samples != VK_SAMPLE_COUNT_1_BIT))
      shader = true;
   if (image->vk.tiling == VK_IMAGE_TILING_LINEAR &&
       (image->vk.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
       rd_image_required_blit_dst(image->vk.format))
      shader = true;
   VkDeviceSize limit = RD_IMAGE_MAX_SIZE;
   if (shader) {
      const struct fdl_image_params parameters = {
         .format = vk_format_to_pipe_format(image->vk.format), .nr_samples = image->vk.samples,
         .width0 = image->vk.extent.width, .height0 = image->vk.extent.height,
         .depth0 = image->vk.extent.depth, .mip_levels = image->vk.mip_levels,
         .array_size = image->vk.array_layers, .is_3d = image->vk.image_type == VK_IMAGE_TYPE_3D,
      };
      fdl5_layout_image(&image->native_layout, &parameters);
      image->native_layout.mip_levels = image->vk.mip_levels;
      if (!image->native_layout.size || image->native_layout.size > limit)
         return false;
      image->size = image->native_layout.size;
      /* The texture unit fetches past the end of small 3D images (IOMMU
       * faults a few pages after the BO in pipeline.monolithic.image 3d
       * sampling); give 3D images slack so those reads stay mapped. */
       if (image->vk.image_type == VK_IMAGE_TYPE_3D) {
          if (image->size > limit - 64 * 1024)
             return false;
          image->size += 64 * 1024;
       }
      for (uint32_t level = 0; level < image->vk.mip_levels; level++) {
         VkExtent3D extent = vk_image_mip_level_extent(&image->vk, level);
         struct rd_image_level *layout = &image->levels[level];
         layout->offset = fdl_surface_offset(&image->native_layout, level, 0);
         layout->row_pitch = fdl_pitch(&image->native_layout, level);
         layout->depth_pitch = image->native_layout.slices[level].size0;
         layout->size = layout->depth_pitch * extent.depth;
         layout->array_pitch = image->native_layout.layer_first ?
            image->native_layout.layer_size : layout->size;
         if (!layout->row_pitch || !layout->array_pitch || layout->row_pitch > UINT32_MAX ||
             layout->depth_pitch > UINT32_MAX || layout->array_pitch > UINT32_MAX ||
             layout->offset > image->size || layout->size > image->size - layout->offset ||
             (image->vk.array_layers - 1) >
                (image->size - layout->offset - layout->size) / layout->array_pitch)
            return false;
      }
      return true;
   }
   for (uint32_t level = 0; level < image->vk.mip_levels; level++) {
      VkExtent3D extent = vk_image_mip_level_extent(&image->vk, level);
      struct rd_image_level *layout = &image->levels[level];
      layout->offset = size;
      VkExtent3D elements = vk_image_extent_to_elements(&image->vk, extent);
      layout->row_pitch = align64((uint64_t)elements.width * image->texel_size * image->vk.samples, 64);
      layout->depth_pitch = layout->row_pitch * elements.height;
      layout->array_pitch = layout->depth_pitch * extent.depth;
      layout->size = layout->array_pitch;
      VkDeviceSize level_size = layout->array_pitch * image->vk.array_layers;
      if (level_size > limit - size)
         return false;
      size += level_size;
   }
   image->size = size;
   return true;
}

static bool
rd_gtk_rgb_import(const VkImageCreateInfo *info,
                  const VkImageDrmFormatModifierExplicitCreateInfoEXT *modifier,
                  const VkExternalMemoryImageCreateInfo *external,
                  VkImageCreateFlags2KHR flags)
{
   return modifier && modifier->drmFormatModifier == DRM_FORMAT_MOD_LINEAR &&
      modifier->drmFormatModifierPlaneCount == 1 && modifier->pPlaneLayouts &&
      external && external->handleTypes == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT &&
      rd_drm_modifier_format_supported(info->format) && !rd_ycbcr_format_supported(info->format) &&
      info->imageType == VK_IMAGE_TYPE_2D && info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT &&
      info->samples == VK_SAMPLE_COUNT_1_BIT && info->mipLevels == 1 && info->arrayLayers == 1 &&
      info->sharingMode == VK_SHARING_MODE_EXCLUSIVE && !flags &&
      (vk_image_usage_flags(info) & VK_IMAGE_USAGE_SAMPLED_BIT) &&
      !(vk_image_usage_flags(info) & ~(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) &&
      debug_get_bool_option("RADISH_GTK_YCBCR_COMPAT", false);
}

VKAPI_ATTR VkResult VKAPI_CALL
rd_CreateImage(VkDevice handle, const VkImageCreateInfo *info,
               const VkAllocationCallbacks *allocator, VkImage *out)
{
   VK_FROM_HANDLE(rd_device, device, handle);
   *out = VK_NULL_HANDLE;
   if (wsi_common_is_swapchain_image(info))
      return wsi_common_create_swapchain_image(device->vk.physical->wsi_device,
                                               info, out);
   VkImageCreateFlags2KHR flags = vk_image_create_flags(info);
   const VkImageDrmFormatModifierListCreateInfoEXT *modifier_list =
      vk_find_struct_const(info->pNext, IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT);
   const VkImageDrmFormatModifierExplicitCreateInfoEXT *modifier_explicit =
      vk_find_struct_const(info->pNext, IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT);
   const VkExternalMemoryImageCreateInfo *external =
      vk_find_struct_const(info->pNext, EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
   uint64_t modifier = DRM_FORMAT_MOD_INVALID;
   bool ycbcr = rd_ycbcr_format_supported(info->format);
   if (info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
      if (info->imageType != VK_IMAGE_TYPE_2D || info->samples != VK_SAMPLE_COUNT_1_BIT ||
          info->mipLevels != 1 || info->arrayLayers != 1 ||
          !rd_drm_modifier_format_supported(info->format) ||
          (!!modifier_list == !!modifier_explicit))
         return VK_ERROR_FORMAT_NOT_SUPPORTED;

      if (modifier_list) {
         if (!modifier_list->drmFormatModifierCount || !modifier_list->pDrmFormatModifiers)
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
         for (uint32_t i = 0; i < modifier_list->drmFormatModifierCount; i++) {
            if (modifier_list->pDrmFormatModifiers[i] == DRM_FORMAT_MOD_LINEAR) {
               modifier = DRM_FORMAT_MOD_LINEAR;
               break;
            }
         }
      } else if (modifier_explicit->drmFormatModifier == DRM_FORMAT_MOD_LINEAR &&
                 modifier_explicit->drmFormatModifierPlaneCount == vk_format_get_plane_count(info->format) &&
                 modifier_explicit->pPlaneLayouts) {
         modifier = DRM_FORMAT_MOD_LINEAR;
      }

      if (modifier == DRM_FORMAT_MOD_INVALID)
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
   } else if (modifier_list || modifier_explicit) {
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }

   if (!rd_image_parameters(info->format, info->imageType, info->tiling,
                            vk_image_usage_flags(info), flags) ||
       (info->samples != VK_SAMPLE_COUNT_1_BIT && info->samples != VK_SAMPLE_COUNT_4_BIT) ||
       !(info->samples & rd_image_sample_counts(info->format, info->imageType, info->tiling,
                                               vk_image_usage_flags(info), flags)) ||
       (info->samples != VK_SAMPLE_COUNT_1_BIT && info->mipLevels != 1) || !info->mipLevels ||
       info->mipLevels > RD_IMAGE_MAX_LEVELS || !info->arrayLayers ||
       info->arrayLayers > 256 || !info->extent.width || !info->extent.height ||
       !info->extent.depth ||
       (info->initialLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
        info->initialLayout != VK_IMAGE_LAYOUT_PREINITIALIZED &&
         !(modifier == DRM_FORMAT_MOD_LINEAR && info->initialLayout == VK_IMAGE_LAYOUT_GENERAL &&
           (ycbcr || rd_gtk_rgb_import(info, modifier_explicit, external, flags)))))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (ycbcr && (info->samples != VK_SAMPLE_COUNT_1_BIT || info->mipLevels != 1 || info->arrayLayers != 1))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (ycbcr) {
      const struct vk_format_ycbcr_info *format_info = vk_format_get_ycbcr_info(info->format);
      if (info->extent.width % format_info->planes[1].denominator_scales[0] ||
          info->extent.height % format_info->planes[1].denominator_scales[1])
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }
   if ((flags & VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT) &&
       (info->mipLevels != 1 || info->arrayLayers != 1))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   uint32_t side = rd_image_max_side(info->imageType, flags);
   if (info->extent.width > side || info->extent.height > side || info->extent.depth > side ||
       (info->imageType == VK_IMAGE_TYPE_1D && (info->extent.height != 1 || info->extent.depth != 1)) ||
       (info->imageType == VK_IMAGE_TYPE_2D && info->extent.depth != 1) ||
       (info->imageType == VK_IMAGE_TYPE_3D && info->arrayLayers != 1) ||
       info->mipLevels > util_logbase2(MAX3(info->extent.width, info->extent.height, info->extent.depth)) + 1 ||
       ((flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) &&
        (info->extent.width != info->extent.height || info->arrayLayers < 6)))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (external && external->handleTypes &&
       (modifier != DRM_FORMAT_MOD_LINEAR ||
        (external->handleTypes & ~VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)))
      return VK_ERROR_FEATURE_NOT_PRESENT;
   struct rd_image *image = vk_image_create(&device->vk, info, allocator, sizeof(*image));
   if (!image)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   image->vk.drm_format_mod = modifier;
   image->texel_size = rd_image_texel_size(ycbcr ? vk_format_get_plane_format(info->format, 0) : info->format);
   if (!rd_image_layout(image)) {
      vk_image_destroy(&device->vk, allocator, &image->vk);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   if (modifier_explicit && ycbcr) {
      VkDeviceSize size = 0;
      uint32_t count = vk_format_get_plane_count(info->format);
      for (uint32_t plane = 0; plane < count; plane++) {
         const VkSubresourceLayout *explicit = &modifier_explicit->pPlaneLayouts[plane];
         struct fdl_layout *native = plane ? &image->extra_planes[plane - 1].native_layout : &image->native_layout;
         struct rd_image_level *level = plane ? &image->extra_planes[plane - 1].levels[0] : &image->levels[0];
         VkExtent3D extent = rd_image_plane_extent(image, plane, 0);
         uint32_t cpp = vk_format_get_blocksize(vk_format_get_plane_format(info->format, plane));
         uint64_t row_bytes = (uint64_t)extent.width * cpp;
         if (!explicit->rowPitch || explicit->rowPitch < row_bytes || explicit->rowPitch > 0x3fffffu ||
             explicit->rowPitch % (1u << native->pitchalign) || explicit->offset % 64 ||
             explicit->offset > RD_IMAGE_MAX_SIZE ||
             extent.height > (RD_IMAGE_MAX_SIZE - explicit->offset) / explicit->rowPitch) {
            vk_image_destroy(&device->vk, allocator, &image->vk);
            return VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT;
         }
         uint64_t size_bytes = explicit->rowPitch * extent.height;
         for (uint32_t previous = 0; previous < plane; previous++) {
            const struct rd_image_level *other = rd_image_plane_level(image, previous, 0);
            if (explicit->offset < other->offset + other->size && other->offset < explicit->offset + size_bytes) {
               vk_image_destroy(&device->vk, allocator, &image->vk);
               return VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT;
            }
         }
         *level = (struct rd_image_level) {
            .offset = explicit->offset, .row_pitch = explicit->rowPitch, .depth_pitch = size_bytes,
            .array_pitch = size_bytes, .size = size_bytes,
         };
         native->pitch0 = explicit->rowPitch;
         native->slices[0].size0 = size_bytes;
         native->layer_size = size_bytes;
         native->size = size_bytes;
         size = MAX2(size, explicit->offset + size_bytes);
      }
      image->size = size;
   } else if (modifier_explicit) {
      const VkSubresourceLayout *plane = &modifier_explicit->pPlaneLayouts[0];
      const uint64_t row_bytes = (uint64_t)image->vk.extent.width * image->texel_size;
      const uint64_t min_depth_pitch = (uint64_t)plane->rowPitch * image->vk.extent.height;
      const uint64_t plane_size = plane->size ? plane->size : min_depth_pitch;
      const uint64_t pitch_alignment = 1u << image->native_layout.pitchalign;
      if (!plane->rowPitch || plane->rowPitch < row_bytes ||
           plane->rowPitch > 0x3fffffu || plane->rowPitch % pitch_alignment ||
          plane->offset % 64 || plane->offset > RD_IMAGE_MAX_SIZE ||
          plane_size > RD_IMAGE_MAX_SIZE - plane->offset ||
          (plane->size && plane->size < (uint64_t)plane->rowPitch * (image->vk.extent.height - 1) + row_bytes) ||
          (plane->depthPitch && plane->depthPitch < min_depth_pitch)) {
          if (debug_get_bool_option("RADISH_DEBUG_ERRORS", false))
            fprintf(stderr, "radish: DRM plane rejected: modifier %llx extent %ux%u cpp %u offset %llu size %llu row %llu array %llu depth %llu min-row %llu min-depth %llu alignment %llu\n",
                  (unsigned long long)modifier, image->vk.extent.width, image->vk.extent.height,
                  image->texel_size, (unsigned long long)plane->offset,
                  (unsigned long long)plane->size, (unsigned long long)plane->rowPitch,
                  (unsigned long long)plane->arrayPitch, (unsigned long long)plane->depthPitch,
                  (unsigned long long)row_bytes, (unsigned long long)min_depth_pitch,
                  (unsigned long long)pitch_alignment);
         vk_image_destroy(&device->vk, allocator, &image->vk);
         return VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT;
      }
      struct rd_image_level *level = &image->levels[0];
      level->offset = plane->offset;
      level->row_pitch = plane->rowPitch;
      level->depth_pitch = plane->depthPitch ? plane->depthPitch : min_depth_pitch;
      level->array_pitch = plane->arrayPitch ? plane->arrayPitch : plane_size;
      level->size = plane_size;
      image->size = plane->offset + plane_size;
      image->native_layout.pitch0 = plane->rowPitch;
      image->native_layout.slices[0].size0 = level->depth_pitch;
      image->native_layout.layer_size = level->array_pitch;
      image->native_layout.size = image->size;
   }
   *out = rd_image_to_handle(image);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
rd_DestroyImage(VkDevice handle, VkImage image_handle, const VkAllocationCallbacks *allocator)
{
   VK_FROM_HANDLE(rd_device, device, handle);
   VK_FROM_HANDLE(rd_image, image, image_handle);
   if (image)
      vk_image_destroy(&device->vk, allocator, &image->vk);
}

VKAPI_ATTR void VKAPI_CALL
rd_GetImageMemoryRequirements2(VkDevice handle, const VkImageMemoryRequirementsInfo2 *info,
                              VkMemoryRequirements2 *out)
{
   VK_FROM_HANDLE(rd_device, device, handle);
   VK_FROM_HANDLE(rd_image, image, info->image);
   struct rd_physical_device *physical =
      container_of(device->vk.physical, struct rd_physical_device, vk);
   bool cached = physical->has_cached_memory && !image->vk.external_handle_types &&
      image->vk.image_type == VK_IMAGE_TYPE_2D &&
      image->vk.tiling == VK_IMAGE_TILING_LINEAR &&
      image->vk.usage == VK_IMAGE_USAGE_TRANSFER_DST_BIT &&
      rd_image_required_blit_dst(image->vk.format);
   out->memoryRequirements = (VkMemoryRequirements) {
      .size = image->size, .alignment = 64, .memoryTypeBits = cached ? 3 : 1,
   };
   vk_foreach_struct(sType, next, out->pNext) {
      if (sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
         VkMemoryDedicatedRequirements *dedicated = (VkMemoryDedicatedRequirements *)next;
         dedicated->prefersDedicatedAllocation = false;
         dedicated->requiresDedicatedAllocation = false;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
rd_GetImageMemoryRequirements(VkDevice handle, VkImage image, VkMemoryRequirements *out)
{
   VkImageMemoryRequirementsInfo2 info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = image,
   };
   VkMemoryRequirements2 result = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
   rd_GetImageMemoryRequirements2(handle, &info, &result);
   *out = result.memoryRequirements;
}

static struct rd_memory *
rd_image_bind_memory(const VkBindImageMemoryInfo *info)
{
   const VkBindImageMemorySwapchainInfoKHR *swapchain =
      vk_find_struct_const(info->pNext, BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR);
   if (swapchain && swapchain->swapchain) {
      if (info->memoryOffset)
         return NULL;
      return rd_memory_from_handle(wsi_common_get_memory(swapchain->swapchain,
                                                         swapchain->imageIndex));
   }
   return rd_memory_from_handle(info->memory);
}

VKAPI_ATTR VkResult VKAPI_CALL
rd_BindImageMemory2(VkDevice handle, uint32_t count, const VkBindImageMemoryInfo *infos)
{
   VK_FROM_HANDLE(rd_device, device, handle);
   for (uint32_t i = 0; i < count; i++) {
      VK_FROM_HANDLE(rd_image, image, infos[i].image);
      for (uint32_t j = 0; j < i; j++)
         if (infos[j].image == infos[i].image)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      struct rd_memory *memory = rd_image_bind_memory(&infos[i]);
      const VkBindImageMemoryDeviceGroupInfo *group =
         vk_find_struct_const(infos[i].pNext, BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO);
      if (group) {
         if (group->deviceIndexCount > 1 || group->splitInstanceBindRegionCount > 1 ||
             (group->deviceIndexCount && group->splitInstanceBindRegionCount) ||
             (group->deviceIndexCount && (!group->pDeviceIndices || group->pDeviceIndices[0])))
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
         if (group->splitInstanceBindRegionCount) {
            if (!image || !(image->vk.create_flags & VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT) ||
                !group->pSplitInstanceBindRegions)
               return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            const VkRect2D *region = group->pSplitInstanceBindRegions;
            if (region->offset.x || region->offset.y ||
                region->extent.width != image->vk.extent.width ||
                region->extent.height != image->vk.extent.height)
               return VK_ERROR_OUT_OF_DEVICE_MEMORY;
         }
      }
      if (!image || !memory || image->vk.base.device != &device->vk ||
          memory->vk.base.device != &device->vk || image->memory || (infos[i].memoryOffset & 63) ||
          infos[i].memoryOffset > memory->vk.size || image->size > memory->vk.size - infos[i].memoryOffset)
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   for (uint32_t i = 0; i < count; i++) {
      VK_FROM_HANDLE(rd_image, image, infos[i].image);
      image->memory = rd_image_bind_memory(&infos[i]);
      image->memory_offset = infos[i].memoryOffset;
   }
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
rd_BindImageMemory(VkDevice handle, VkImage image, VkDeviceMemory memory, VkDeviceSize offset)
{
   VkBindImageMemoryInfo info = {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
                               .image = image, .memory = memory, .memoryOffset = offset};
   return rd_BindImageMemory2(handle, 1, &info);
}

VKAPI_ATTR void VKAPI_CALL
rd_GetImageSparseMemoryRequirements(VkDevice handle, VkImage image, uint32_t *count,
                                   VkSparseImageMemoryRequirements *out)
{
   *count = 0;
}

VKAPI_ATTR void VKAPI_CALL
rd_GetImageSparseMemoryRequirements2(VkDevice handle, const VkImageSparseMemoryRequirementsInfo2 *info,
                                    uint32_t *count, VkSparseImageMemoryRequirements2 *out)
{
   *count = 0;
}

VKAPI_ATTR void VKAPI_CALL
rd_GetImageSubresourceLayout(VkDevice handle, VkImage image_handle,
                            const VkImageSubresource *subresource, VkSubresourceLayout *out)
{
   VK_FROM_HANDLE(rd_image, image, image_handle);
   *out = (VkSubresourceLayout){0};
   if (!image)
      return;
   uint32_t plane = rd_image_aspect_plane(image->vk.format, subresource->aspectMask);
   bool modifier_plane = image->vk.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT &&
      (subresource->aspectMask == VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT ||
       subresource->aspectMask == VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT ||
       subresource->aspectMask == VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT);
   if (modifier_plane)
      plane = subresource->aspectMask == VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT ? 0 :
              subresource->aspectMask == VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT ? 1 : 2;
   if (!subresource->aspectMask ||
       (!modifier_plane && (subresource->aspectMask & ~vk_format_aspects(image->vk.format))) ||
       (subresource->aspectMask & (subresource->aspectMask - 1)) ||
       plane >= vk_format_get_plane_count(image->vk.format) ||
       subresource->mipLevel >= image->vk.mip_levels || subresource->arrayLayer >= image->vk.array_layers)
      return;
   const struct rd_image_level *level = rd_image_plane_level(image, plane, subresource->mipLevel);
   *out = (VkSubresourceLayout) {
      .offset = level->offset + subresource->arrayLayer * level->array_pitch,
      .size = level->size, .rowPitch = level->row_pitch,
      .arrayPitch = level->array_pitch, .depthPitch = level->depth_pitch,
   };
}

VKAPI_ATTR VkResult VKAPI_CALL
rd_CreateImageView(VkDevice handle, const VkImageViewCreateInfo *info,
                   const VkAllocationCallbacks *allocator, VkImageView *out)
{
   VK_FROM_HANDLE(rd_device, device, handle);
   VK_FROM_HANDLE(rd_image, image, info->image);
   *out = VK_NULL_HANDLE;
   VkImageUsageFlags2KHR view_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
   const VkImageSubresourceRange *r = &info->subresourceRange;
   bool ycbcr = image && rd_ycbcr_format_supported(image->vk.format);
   bool converted = ycbcr && r->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT;
   uint32_t plane = ycbcr && !converted ? rd_image_aspect_plane(image->vk.format, r->aspectMask) : 0;
   const VkSamplerYcbcrConversionInfo *conversion_info =
      vk_find_struct_const(info->pNext, SAMPLER_YCBCR_CONVERSION_INFO);
   VK_FROM_HANDLE(vk_ycbcr_conversion, conversion, conversion_info ? conversion_info->conversion : VK_NULL_HANDLE);
   bool block_view = image && (image->vk.create_flags & VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT) &&
      !vk_format_is_compressed(info->format) &&
      vk_format_get_blocksize(info->format) == vk_format_get_blocksize(image->vk.format);
   bool format_compatible = image && (block_view || info->format == image->vk.format ||
      ((image->vk.create_flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) &&
       !vk_format_is_depth_or_stencil(image->vk.format) &&
       !vk_format_is_depth_or_stencil(info->format) &&
        vk_format_get_class_info(info->format) == vk_format_get_class_info(image->vk.format)));
   if (ycbcr) {
      VkFormat plane_format = plane < vk_format_get_plane_count(image->vk.format) ?
         vk_format_get_plane_format(image->vk.format, plane) : VK_FORMAT_UNDEFINED;
      format_compatible = converted ? info->format == image->vk.format && conversion &&
         conversion->state.format == info->format :
         (image->vk.create_flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) && plane_format != VK_FORMAT_UNDEFINED &&
         vk_format_get_class_info(info->format) == vk_format_get_class_info(plane_format) && !conversion;
   }
   if (!image || info->flags || !format_compatible ||
       !(image->vk.usage & view_usage) || !r->aspectMask ||
       (!converted && (r->aspectMask & ~vk_format_aspects(image->vk.format))) ||
       r->baseMipLevel >= image->vk.mip_levels)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (converted &&
       ((info->components.r != VK_COMPONENT_SWIZZLE_IDENTITY && info->components.r != VK_COMPONENT_SWIZZLE_R) ||
        (info->components.g != VK_COMPONENT_SWIZZLE_IDENTITY && info->components.g != VK_COMPONENT_SWIZZLE_G) ||
        (info->components.b != VK_COMPONENT_SWIZZLE_IDENTITY && info->components.b != VK_COMPONENT_SWIZZLE_B) ||
        (info->components.a != VK_COMPONENT_SWIZZLE_IDENTITY && info->components.a != VK_COMPONENT_SWIZZLE_A)))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   bool slice_view = image->vk.image_type == VK_IMAGE_TYPE_3D &&
      (image->vk.create_flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT) &&
      (info->viewType == VK_IMAGE_VIEW_TYPE_2D || info->viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
   uint32_t max_layers = slice_view ?
      vk_image_mip_level_extent(&image->vk, r->baseMipLevel).depth : image->vk.array_layers;
   if (r->baseArrayLayer >= max_layers)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   uint32_t levels = vk_image_subresource_level_count(&image->vk, r);
   uint32_t layers = r->layerCount == VK_REMAINING_ARRAY_LAYERS ?
      max_layers - r->baseArrayLayer : r->layerCount;
   if (!levels || levels > image->vk.mip_levels - r->baseMipLevel || !layers ||
       layers > max_layers - r->baseArrayLayer || (slice_view && levels != 1) ||
       (block_view && (levels != 1 || layers != 1)))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   bool compatible = (image->vk.image_type == VK_IMAGE_TYPE_1D &&
      (info->viewType == VK_IMAGE_VIEW_TYPE_1D || info->viewType == VK_IMAGE_VIEW_TYPE_1D_ARRAY)) ||
      (image->vk.image_type == VK_IMAGE_TYPE_2D &&
      (info->viewType == VK_IMAGE_VIEW_TYPE_2D || info->viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY)) ||
      (image->vk.image_type == VK_IMAGE_TYPE_3D && info->viewType == VK_IMAGE_VIEW_TYPE_3D) ||
      slice_view;
   if ((info->viewType == VK_IMAGE_VIEW_TYPE_CUBE || info->viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) &&
       image->vk.image_type == VK_IMAGE_TYPE_2D &&
       (image->vk.create_flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) &&
       (info->viewType == VK_IMAGE_VIEW_TYPE_CUBE ? layers == 6 : !(layers % 6)))
      compatible = true;
   const VkImageViewUsageCreateInfo *usage_info =
      vk_find_struct_const(info->pNext, IMAGE_VIEW_USAGE_CREATE_INFO);
   VkImageUsageFlags2KHR usage = usage_info ? usage_info->usage : image->vk.usage;
   if ((usage & ~image->vk.usage) || (!converted && !rd_shader_format_supported(info->format)) ||
       ((usage & VK_IMAGE_USAGE_STORAGE_BIT) && !rd_storage_shader_format_supported(info->format)) ||
       (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) &
        ~rd_image_shader_usage(info->format)))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (!compatible || ((info->viewType == VK_IMAGE_VIEW_TYPE_1D || info->viewType == VK_IMAGE_VIEW_TYPE_2D) && layers != 1))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   struct rd_image_view *view = vk_image_view_create(&device->vk, info, allocator, sizeof(*view));
   if (!view)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   view->plane = plane;
   view->ycbcr = converted;
   if (converted)
      view->conversion = conversion->state;
   else if (ycbcr)
      view->vk.extent = rd_image_plane_extent(image, plane, r->baseMipLevel);
   *out = rd_image_view_to_handle(view);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
rd_DestroyImageView(VkDevice handle, VkImageView view_handle, const VkAllocationCallbacks *allocator)
{
   VK_FROM_HANDLE(rd_device, device, handle);
   VK_FROM_HANDLE(rd_image_view, view, view_handle);
   if (view)
      vk_image_view_destroy(&device->vk, allocator, &view->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
rd_CreateBufferView(VkDevice handle, const VkBufferViewCreateInfo *info,
                    const VkAllocationCallbacks *allocator, VkBufferView *out)
{
   VK_FROM_HANDLE(rd_device, device, handle);
   VK_FROM_HANDLE(rd_buffer, buffer, info->buffer);
   *out = VK_NULL_HANDLE;
   uint32_t size = vk_format_get_blocksize(info->format);
   if (!buffer || info->flags || !rd_shader_format_supported(info->format) ||
       vk_format_is_compressed(info->format) || vk_format_is_srgb(info->format) || vk_format_is_depth_or_stencil(info->format) ||
       !size || info->offset >= buffer->vk.size ||
        (info->offset & 63) || !(buffer->vk.usage &
       (VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   VkDeviceSize range = info->range == VK_WHOLE_SIZE ? buffer->vk.size - info->offset : info->range;
   if (range > buffer->vk.size - info->offset ||
       (info->range != VK_WHOLE_SIZE && (!range || range % size)) ||
       range / size > (1u << 27))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   range -= range % size;
   struct rd_buffer_view *view = vk_buffer_view_create(&device->vk, info, allocator, sizeof(*view));
   if (!view)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   view->vk.range = range;
   view->vk.elements = range / size;
   *out = rd_buffer_view_to_handle(view);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
rd_DestroyBufferView(VkDevice handle, VkBufferView view_handle, const VkAllocationCallbacks *allocator)
{
   VK_FROM_HANDLE(rd_device, device, handle);
   VK_FROM_HANDLE(rd_buffer_view, view, view_handle);
   if (view)
      vk_buffer_view_destroy(&device->vk, allocator, &view->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
rd_CreateSamplerYcbcrConversion(VkDevice handle, const VkSamplerYcbcrConversionCreateInfo *info,
                               const VkAllocationCallbacks *allocator, VkSamplerYcbcrConversion *out)
{
   VK_FROM_HANDLE(rd_device, device, handle);
   *out = VK_NULL_HANDLE;
   if (!rd_ycbcr_format_supported(info->format) ||
       info->ycbcrModel > VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020 ||
       info->ycbcrRange > VK_SAMPLER_YCBCR_RANGE_ITU_NARROW ||
       info->components.r > VK_COMPONENT_SWIZZLE_A || info->components.g > VK_COMPONENT_SWIZZLE_A ||
       info->components.b > VK_COMPONENT_SWIZZLE_A || info->components.a > VK_COMPONENT_SWIZZLE_A ||
       info->xChromaOffset > VK_CHROMA_LOCATION_MIDPOINT || info->yChromaOffset > VK_CHROMA_LOCATION_MIDPOINT ||
       (info->chromaFilter != VK_FILTER_NEAREST && info->chromaFilter != VK_FILTER_LINEAR) ||
       info->forceExplicitReconstruction)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   const struct vk_format_ycbcr_info *format_info = vk_format_get_ycbcr_info(info->format);
   bool subsampled = format_info->planes[1].denominator_scales[0] > 1 ||
                     format_info->planes[1].denominator_scales[1] > 1;
   VkComponentSwizzle mapping[4] = {info->components.r, info->components.g, info->components.b, info->components.a};
   for (uint32_t i = 0; i < ARRAY_SIZE(mapping); i++)
      if (mapping[i] == VK_COMPONENT_SWIZZLE_IDENTITY)
         mapping[i] = VK_COMPONENT_SWIZZLE_R + i;
   if (subsampled &&
       (mapping[1] != VK_COMPONENT_SWIZZLE_G ||
        (mapping[3] != VK_COMPONENT_SWIZZLE_A && mapping[3] != VK_COMPONENT_SWIZZLE_ONE &&
         mapping[3] != VK_COMPONENT_SWIZZLE_ZERO) ||
        !((mapping[0] == VK_COMPONENT_SWIZZLE_R && mapping[2] == VK_COMPONENT_SWIZZLE_B) ||
          (mapping[0] == VK_COMPONENT_SWIZZLE_B && mapping[2] == VK_COMPONENT_SWIZZLE_R))))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (info->ycbcrModel != VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY)
      for (uint32_t i = 0; i < 3; i++)
         if (mapping[i] < VK_COMPONENT_SWIZZLE_R || mapping[i] > VK_COMPONENT_SWIZZLE_B)
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
   struct vk_ycbcr_conversion *conversion = vk_object_zalloc(&device->vk, allocator,
      sizeof(*conversion), VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION);
   if (!conversion)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   conversion->state = (struct vk_ycbcr_conversion_state) {
      .format = info->format, .ycbcr_model = info->ycbcrModel, .ycbcr_range = info->ycbcrRange,
      .mapping = {mapping[0], mapping[1], mapping[2], mapping[3]},
      .chroma_offsets = {info->xChromaOffset, info->yChromaOffset}, .chroma_filter = info->chromaFilter,
      .chroma_reconstruction = subsampled &&
         (info->xChromaOffset == VK_CHROMA_LOCATION_COSITED_EVEN ||
          info->yChromaOffset == VK_CHROMA_LOCATION_COSITED_EVEN),
   };
   *out = vk_ycbcr_conversion_to_handle(conversion);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
rd_CreateSampler(VkDevice handle, const VkSamplerCreateInfo *info,
                  const VkAllocationCallbacks *allocator, VkSampler *out)
{
   VK_FROM_HANDLE(rd_device, device, handle);
   *out = VK_NULL_HANDLE;
   const VkSamplerYcbcrConversionInfo *conversion_info =
      vk_find_struct_const(info->pNext, SAMPLER_YCBCR_CONVERSION_INFO);
   VK_FROM_HANDLE(vk_ycbcr_conversion, conversion, conversion_info ? conversion_info->conversion : VK_NULL_HANDLE);
   if (conversion && (info->magFilter != conversion->state.chroma_filter ||
       info->minFilter != conversion->state.chroma_filter || info->compareEnable || info->unnormalizedCoordinates ||
       info->addressModeU != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE ||
       info->addressModeV != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE ||
       info->addressModeW != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE))
      return VK_ERROR_FEATURE_NOT_PRESENT;
   if (info->flags || info->anisotropyEnable ||
       (info->magFilter != VK_FILTER_NEAREST && info->magFilter != VK_FILTER_LINEAR) ||
       (info->minFilter != VK_FILTER_NEAREST && info->minFilter != VK_FILTER_LINEAR) ||
       info->mipmapMode > VK_SAMPLER_MIPMAP_MODE_LINEAR ||
       info->addressModeU > VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
       info->addressModeV > VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
       info->addressModeW > VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
       info->borderColor > VK_BORDER_COLOR_INT_OPAQUE_WHITE ||
       info->compareOp > VK_COMPARE_OP_ALWAYS || info->minLod > info->maxLod ||
       fabsf(info->mipLodBias) > 16.0f ||
       isnan(info->minLod) || isnan(info->maxLod) || !isfinite(info->mipLodBias)) {
      /* infinite LOD limits are fine: the descriptor clamps them */
      if (debug_get_bool_option("RADISH_DEBUG_ERRORS", false))
         fprintf(stderr, "radish: sampler refused: flags %x aniso %d filters %d %d mip %d address %d %d %d "
                 "border %d compare %d lod %f..%f bias %f\n", info->flags, info->anisotropyEnable,
                 info->magFilter, info->minFilter, info->mipmapMode, info->addressModeU,
                 info->addressModeV, info->addressModeW, info->borderColor, info->compareOp,
                 info->minLod, info->maxLod, info->mipLodBias);
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   struct rd_sampler *sampler = vk_sampler_create(&device->vk, info, allocator, sizeof(*sampler));
   if (!sampler)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   vk_sampler_state_init(&sampler->state, info);
   *out = rd_sampler_to_handle(sampler);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
rd_DestroySampler(VkDevice handle, VkSampler sampler_handle, const VkAllocationCallbacks *allocator)
{
   VK_FROM_HANDLE(rd_device, device, handle);
   VK_FROM_HANDLE(rd_sampler, sampler, sampler_handle);
   if (sampler)
      vk_sampler_destroy(&device->vk, allocator, &sampler->vk);
}

static bool
rd_image_range(const struct rd_image *image, const VkImageSubresourceRange *range)
{
   bool color = image && rd_ycbcr_format_supported(image->vk.format) && range->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT;
   if (!image || !range->aspectMask || (!color && (range->aspectMask & ~vk_format_aspects(image->vk.format))) ||
       range->baseMipLevel >= image->vk.mip_levels)
      return false;
   /* a 3D image's slices may be named as layers (2D views of slices) */
   uint32_t max_layers = image->vk.image_type == VK_IMAGE_TYPE_3D ?
      MAX2(image->vk.array_layers, image->vk.extent.depth) : image->vk.array_layers;
   if (range->baseArrayLayer >= max_layers)
      return false;
   uint32_t levels = vk_image_subresource_level_count(&image->vk, range);
   uint32_t layers = range->layerCount == VK_REMAINING_ARRAY_LAYERS ?
      max_layers - range->baseArrayLayer : range->layerCount;
   return levels && levels <= image->vk.mip_levels - range->baseMipLevel &&
          layers && layers <= max_layers - range->baseArrayLayer;
}

bool
rd_image_barrier_valid(const VkImageMemoryBarrier2 *barrier)
{
   /* Images have one layout whatever the Vulkan layout says, so a
    * transition is only checked for a valid subresource range. */
   VK_FROM_HANDLE(rd_image, image, barrier->image);
   return rd_image_range(image, &barrier->subresourceRange);
}

static bool
rd_image_box(struct rd_image *image, const VkImageSubresourceLayers *subresource,
             VkOffset3D offset, VkExtent3D extent, VkDeviceSize *base,
             VkDeviceSize *row, VkDeviceSize *slice, uint32_t *slices)
{
   uint32_t plane = image ? rd_image_aspect_plane(image->vk.format, subresource->aspectMask) : UINT32_MAX;
   if (!image || !subresource->aspectMask ||
       plane == UINT32_MAX ||
       (subresource->aspectMask & ~vk_format_aspects(image->vk.format)) ||
       subresource->mipLevel >= image->vk.mip_levels || !subresource->layerCount ||
       subresource->baseArrayLayer >= image->vk.array_layers ||
       subresource->layerCount > image->vk.array_layers - subresource->baseArrayLayer ||
       offset.x < 0 || offset.y < 0 || offset.z < 0 || !extent.width || !extent.height || !extent.depth)
      return false;
   VkExtent3D limit = rd_image_plane_extent(image, plane, subresource->mipLevel);
   VkFormat format = rd_ycbcr_format_supported(image->vk.format) ?
      vk_format_get_plane_format(image->vk.format, plane) : image->vk.format;
   uint32_t bw = vk_format_get_blockwidth(format);
   uint32_t bh = vk_format_get_blockheight(format);
   VkExtent3D elements = {DIV_ROUND_UP(limit.width, bw), DIV_ROUND_UP(limit.height, bh), limit.depth};
   uint32_t width = elements.width * bw;
   uint32_t height = elements.height * bh;
   if ((uint32_t)offset.x > width || extent.width > width - offset.x ||
       (uint32_t)offset.y > height || extent.height > height - offset.y ||
       (uint32_t)offset.z > limit.depth || extent.depth > limit.depth - offset.z)
      return false;
   if (offset.x % bw || offset.y % bh ||
       (extent.width % bw && extent.width != limit.width - offset.x) ||
       (extent.height % bh && extent.height != limit.height - offset.y))
      return false;
   VkOffset3D element_offset = {offset.x / bw, offset.y / bh, offset.z};
   const struct rd_image_level *level = rd_image_plane_level(image, plane, subresource->mipLevel);
   *base = level->offset + subresource->baseArrayLayer * level->array_pitch +
           element_offset.z * level->depth_pitch + element_offset.y * level->row_pitch +
            (uint64_t)element_offset.x * vk_format_get_blocksize(format) * image->vk.samples;
   *row = level->row_pitch;
   *slice = image->vk.image_type == VK_IMAGE_TYPE_3D ? level->depth_pitch : level->array_pitch;
   *slices = image->vk.image_type == VK_IMAGE_TYPE_3D ? extent.depth : subresource->layerCount;
   return true;
}

static bool
rd_image_copy_layout(VkImageLayout layout, bool source)
{
   return layout == VK_IMAGE_LAYOUT_GENERAL || layout ==
      (source ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}

static bool
rd_image_command_valid(struct rd_command_buffer *command, bool valid)
{
   if (!valid)
      rd_set_error(&command->vk, VK_ERROR_UNKNOWN);
   return valid;
}

VKAPI_ATTR void VKAPI_CALL
rd_CmdCopyImage(VkCommandBuffer handle, VkImage src_handle, VkImageLayout src_layout,
                VkImage dst_handle, VkImageLayout dst_layout, uint32_t count, const VkImageCopy *regions)
{
   VK_FROM_HANDLE(rd_command_buffer, command, handle);
   VK_FROM_HANDLE(rd_image, src, src_handle);
   VK_FROM_HANDLE(rd_image, dst, dst_handle);
   bool ycbcr = src && dst && (rd_ycbcr_format_supported(src->vk.format) || rd_ycbcr_format_supported(dst->vk.format));
   if (!rd_image_command_valid(command, src && dst && (ycbcr || src->texel_size == dst->texel_size) &&
       src->vk.samples == dst->vk.samples &&
       (!(vk_format_is_depth_or_stencil(src->vk.format) || vk_format_is_depth_or_stencil(dst->vk.format)) ||
        src->vk.format == dst->vk.format) &&
       (src->vk.image_type == dst->vk.image_type ||
        (src->vk.image_type == VK_IMAGE_TYPE_2D && dst->vk.image_type == VK_IMAGE_TYPE_3D) ||
        (src->vk.image_type == VK_IMAGE_TYPE_3D && dst->vk.image_type == VK_IMAGE_TYPE_2D)) &&
       (src->vk.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
       (dst->vk.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
       rd_image_copy_layout(src_layout, true) && rd_image_copy_layout(dst_layout, false)))
      return;
   for (uint32_t i = 0; i < count; i++) {
      struct rd_image_transfer transfer = {.src_image = src, .dst_image = dst};
      if (!rd_image_command_valid(command,
          ycbcr || regions[i].srcSubresource.aspectMask == regions[i].dstSubresource.aspectMask))
         return;
      VkFormat src_format = src->vk.format, dst_format = dst->vk.format;
      if (ycbcr) {
         uint32_t src_plane = rd_image_aspect_plane(src_format, regions[i].srcSubresource.aspectMask);
         uint32_t dst_plane = rd_image_aspect_plane(dst_format, regions[i].dstSubresource.aspectMask);
         if (!rd_image_command_valid(command, src_plane != UINT32_MAX && dst_plane != UINT32_MAX))
            return;
         src_format = vk_format_get_plane_format(src_format, src_plane);
         dst_format = vk_format_get_plane_format(dst_format, dst_plane);
         if (!rd_image_command_valid(command, vk_format_get_blocksize(src_format) == vk_format_get_blocksize(dst_format)))
            return;
      }
      if (src->vk.format == VK_FORMAT_D24_UNORM_S8_UINT) {
         if (regions[i].srcSubresource.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT)
            transfer.copy_mask = 0x00ffffffu;
         else if (regions[i].srcSubresource.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT)
            transfer.copy_mask = 0xff000000u;
      }
      VkExtent3D src_extent = regions[i].extent;
      VkExtent3D dst_extent = regions[i].extent;
      if (src->vk.image_type != dst->vk.image_type) {
         if (src->vk.image_type == VK_IMAGE_TYPE_2D)
            src_extent.depth = 1;
         else
            dst_extent.depth = 1;
      }
      VkExtent3D elements = {DIV_ROUND_UP(src_extent.width, vk_format_get_blockwidth(src_format)),
         DIV_ROUND_UP(src_extent.height, vk_format_get_blockheight(src_format)), src_extent.depth};
      dst_extent.width = elements.width * vk_format_get_blockwidth(dst_format);
      dst_extent.height = elements.height * vk_format_get_blockheight(dst_format);
      uint32_t src_slices, dst_slices;
      if (!rd_image_command_valid(command,
          rd_image_box(src, &regions[i].srcSubresource, regions[i].srcOffset, src_extent,
             &transfer.src_offset, &transfer.src_row_pitch, &transfer.src_slice_pitch, &src_slices) &&
          rd_image_box(dst, &regions[i].dstSubresource, regions[i].dstOffset, dst_extent,
             &transfer.dst_offset, &transfer.dst_row_pitch, &transfer.dst_slice_pitch, &dst_slices) && src_slices == dst_slices))
         return;
      transfer.row_bytes = (uint64_t)elements.width * vk_format_get_blocksize(src_format) * src->vk.samples;
      transfer.rows = elements.height;
      transfer.slices = src_slices;
      struct rd_command *recorded = rd_command_append(command, RD_CMD_IMAGE_COPY);
      if (!recorded)
         return;
      recorded->image_transfer = transfer;
   }
}

VKAPI_ATTR void VKAPI_CALL
rd_CmdCopyImage2(VkCommandBuffer handle, const VkCopyImageInfo2 *info)
{
   VK_FROM_HANDLE(rd_command_buffer, command, handle);
   for (uint32_t i = 0; i < info->regionCount; i++) {
      const VkImageCopy2 *region = &info->pRegions[i];
      VkImageCopy copy = {
         .srcSubresource = region->srcSubresource,
         .srcOffset = region->srcOffset,
         .dstSubresource = region->dstSubresource,
         .dstOffset = region->dstOffset,
         .extent = region->extent,
      };
      rd_CmdCopyImage(handle, info->srcImage, info->srcImageLayout,
                      info->dstImage, info->dstImageLayout, 1, &copy);
      if (command->vk.record_result != VK_SUCCESS)
         return;
   }
}

static void
rd_record_buffer_image(struct rd_command_buffer *command, struct rd_buffer *buffer,
                       struct rd_image *image, VkImageLayout layout, bool to_image,
                       uint32_t count, const VkBufferImageCopy *regions)
{
   if (!rd_image_command_valid(command, buffer && image && image->vk.samples == VK_SAMPLE_COUNT_1_BIT &&
       (buffer->vk.usage & (to_image ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_TRANSFER_DST_BIT)) &&
       (image->vk.usage & (to_image ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) &&
       rd_image_copy_layout(layout, !to_image)))
      return;
   for (uint32_t i = 0; i < count; i++) {
      const VkBufferImageCopy *region = &regions[i];
      uint32_t plane = rd_image_aspect_plane(image->vk.format, region->imageSubresource.aspectMask);
      if (!rd_image_command_valid(command, plane != UINT32_MAX))
         return;
      VkFormat format = rd_ycbcr_format_supported(image->vk.format) ?
         vk_format_get_plane_format(image->vk.format, plane) : image->vk.format;
      uint32_t cpp = vk_format_get_blocksize(format);
      /* Depth/stencil: single-aspect formats copy texels as they are stored.
       * The depth aspect of D24S8 is stored and copied as X8_D24 words; on
       * the way in, the stencil byte of each word is preserved. The stencil
       * aspect of D24S8 would need repacking and is not supported.
       */
      uint32_t copy_mask = 0;
      bool stencil_lane = false;
      if (image->vk.format == VK_FORMAT_D24_UNORM_S8_UINT) {
         if (region->imageSubresource.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT)
            stencil_lane = true;
         else if (!rd_image_command_valid(command,
             region->imageSubresource.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT))
            return;
         copy_mask = to_image && !stencil_lane ? 0x00ffffffu : 0;
      }
      VkDeviceSize image_offset, image_row, image_slice;
      uint32_t slices;
      if (!rd_image_command_valid(command,
          rd_image_box(image, &region->imageSubresource, region->imageOffset, region->imageExtent,
             &image_offset, &image_row, &image_slice, &slices)))
         return;
      uint64_t row_length = region->bufferRowLength ? region->bufferRowLength : region->imageExtent.width;
      uint64_t height = region->bufferImageHeight ? region->bufferImageHeight : region->imageExtent.height;
      uint32_t bw = vk_format_get_blockwidth(format);
      uint32_t bh = vk_format_get_blockheight(format);
      uint64_t row = DIV_ROUND_UP(row_length, bw) * cpp;
      if (!rd_image_command_valid(command, row_length >= region->imageExtent.width &&
          height >= region->imageExtent.height && row <= RD_IMAGE_MAX_SIZE &&
          (!region->bufferRowLength || !(region->bufferRowLength % bw)) &&
          (!region->bufferImageHeight || !(region->bufferImageHeight % bh)) &&
          (slices == 1 || DIV_ROUND_UP(height, bh) <= RD_IMAGE_MAX_SIZE / row) &&
           !(region->bufferOffset % cpp)))
         return;
      VkBufferImageCopy2 region2 = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
         .bufferOffset = region->bufferOffset,
         .bufferRowLength = region->bufferRowLength,
         .bufferImageHeight = region->bufferImageHeight,
         .imageSubresource = region->imageSubresource,
         .imageOffset = region->imageOffset,
         .imageExtent = region->imageExtent,
      };
      struct vk_image_buffer_layout buffer_layout = vk_image_buffer_copy_layout(&image->vk, &region2);
      VkExtent3D elements = {DIV_ROUND_UP(region->imageExtent.width, bw),
         DIV_ROUND_UP(region->imageExtent.height, bh), region->imageExtent.depth};
      row = buffer_layout.row_stride_B;
      uint64_t slice = buffer_layout.image_stride_B;
      uint64_t rows = elements.height;
      uint64_t row_bytes = (uint64_t)elements.width * buffer_layout.element_size_B;
      uint64_t end = vk_image_buffer_range(&image->vk, &buffer_layout, &elements, &region->imageSubresource);
      if (!rd_image_command_valid(command, region->bufferOffset <= buffer->vk.size &&
          end <= buffer->vk.size - region->bufferOffset))
         return;
      struct rd_image_transfer transfer = {
         .src_image = to_image ? NULL : image, .dst_image = to_image ? image : NULL,
         .src_buffer = to_image ? buffer : NULL, .dst_buffer = to_image ? NULL : buffer,
         .src_offset = to_image ? region->bufferOffset : image_offset,
         .dst_offset = to_image ? image_offset : region->bufferOffset,
         .src_row_pitch = to_image ? row : image_row, .dst_row_pitch = to_image ? image_row : row,
         .src_slice_pitch = to_image ? slice : image_slice, .dst_slice_pitch = to_image ? image_slice : slice,
         .row_bytes = row_bytes, .rows = rows, .slices = slices, .copy_mask = copy_mask,
      };
      if (stencil_lane) {
         /* Buffer: one stencil byte per texel. Image: byte 3 of each word. */
         transfer.lane_step = 1;
         transfer.src_step = to_image ? 1 : 4;
         transfer.dst_step = to_image ? 4 : 1;
         transfer.src_lane = to_image ? 0 : 3;
         transfer.dst_lane = to_image ? 3 : 0;
         transfer.row_bytes = elements.width;
      }
      struct rd_command *recorded = rd_command_append(command, RD_CMD_IMAGE_COPY);
      if (!recorded)
         return;
      recorded->image_transfer = transfer;
   }
}

VKAPI_ATTR void VKAPI_CALL
rd_CmdCopyBufferToImage(VkCommandBuffer handle, VkBuffer src, VkImage dst, VkImageLayout layout,
                       uint32_t count, const VkBufferImageCopy *regions)
{
   VK_FROM_HANDLE(rd_command_buffer, command, handle);
   rd_record_buffer_image(command, rd_buffer_from_handle(src), rd_image_from_handle(dst), layout, true, count, regions);
}

VKAPI_ATTR void VKAPI_CALL
rd_CmdCopyImageToBuffer(VkCommandBuffer handle, VkImage src, VkImageLayout layout, VkBuffer dst,
                       uint32_t count, const VkBufferImageCopy *regions)
{
   VK_FROM_HANDLE(rd_command_buffer, command, handle);
   rd_record_buffer_image(command, rd_buffer_from_handle(dst), rd_image_from_handle(src), layout, false, count, regions);
}

VKAPI_ATTR void VKAPI_CALL
rd_CmdCopyBufferToImage2(VkCommandBuffer handle, const VkCopyBufferToImageInfo2 *info)
{
   VK_FROM_HANDLE(rd_command_buffer, command, handle);
   for (uint32_t i = 0; i < info->regionCount; i++) {
      const VkBufferImageCopy2 *region = &info->pRegions[i];
      VkBufferImageCopy copy = {
         .bufferOffset = region->bufferOffset,
         .bufferRowLength = region->bufferRowLength,
         .bufferImageHeight = region->bufferImageHeight,
         .imageSubresource = region->imageSubresource,
         .imageOffset = region->imageOffset,
         .imageExtent = region->imageExtent,
      };
      rd_CmdCopyBufferToImage(handle, info->srcBuffer, info->dstImage,
                              info->dstImageLayout, 1, &copy);
      if (command->vk.record_result != VK_SUCCESS)
         return;
   }
}

VKAPI_ATTR void VKAPI_CALL
rd_CmdCopyImageToBuffer2(VkCommandBuffer handle, const VkCopyImageToBufferInfo2 *info)
{
   VK_FROM_HANDLE(rd_command_buffer, command, handle);
   for (uint32_t i = 0; i < info->regionCount; i++) {
      const VkBufferImageCopy2 *region = &info->pRegions[i];
      VkBufferImageCopy copy = {
         .bufferOffset = region->bufferOffset,
         .bufferRowLength = region->bufferRowLength,
         .bufferImageHeight = region->bufferImageHeight,
         .imageSubresource = region->imageSubresource,
         .imageOffset = region->imageOffset,
         .imageExtent = region->imageExtent,
      };
      rd_CmdCopyImageToBuffer(handle, info->srcImage, info->srcImageLayout,
                              info->dstBuffer, 1, &copy);
      if (command->vk.record_result != VK_SUCCESS)
         return;
   }
}

VKAPI_ATTR void VKAPI_CALL
rd_CmdClearColorImage(VkCommandBuffer handle, VkImage image_handle, VkImageLayout layout,
                      const VkClearColorValue *color, uint32_t count, const VkImageSubresourceRange *ranges)
{
   VK_FROM_HANDLE(rd_command_buffer, command, handle);
   VK_FROM_HANDLE(rd_image, image, image_handle);
   if (!rd_image_command_valid(command, image && !vk_format_is_depth_or_stencil(image->vk.format) &&
       !rd_ycbcr_format_supported(image->vk.format) &&
       !vk_format_is_compressed(image->vk.format) &&
       (image->vk.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
       rd_image_copy_layout(layout, false)))
      return;
   uint8_t clear[16] = {0};
   util_format_pack_rgba(vk_format_to_pipe_format(image->vk.format), clear, color, 1);
   for (uint32_t i = 0; i < count; i++) {
      if (!rd_image_command_valid(command, rd_image_range(image, &ranges[i]) &&
          ranges[i].baseArrayLayer < image->vk.array_layers &&
          vk_image_subresource_layer_count(&image->vk, &ranges[i]) <=
             image->vk.array_layers - ranges[i].baseArrayLayer))
         return;
      uint32_t levels = vk_image_subresource_level_count(&image->vk, &ranges[i]);
      uint32_t layers = vk_image_subresource_layer_count(&image->vk, &ranges[i]);
      for (uint32_t m = 0; m < levels; m++) {
         uint32_t level = ranges[i].baseMipLevel + m;
         VkExtent3D extent = vk_image_mip_level_extent(&image->vk, level);
         struct rd_image_transfer transfer = {
            .dst_image = image,
            .dst_offset = image->levels[level].offset + ranges[i].baseArrayLayer * image->levels[level].array_pitch,
            .dst_row_pitch = image->levels[level].row_pitch,
            .dst_slice_pitch = image->vk.image_type == VK_IMAGE_TYPE_3D ? image->levels[level].depth_pitch : image->levels[level].array_pitch,
            .row_bytes = (uint64_t)extent.width * image->texel_size * image->vk.samples,
            .rows = extent.height, .slices = image->vk.image_type == VK_IMAGE_TYPE_3D ? extent.depth : layers,
            .texel_size = image->texel_size,
         };
         memcpy(transfer.clear, clear, sizeof(clear));
         struct rd_command *recorded = rd_command_append(command, RD_CMD_IMAGE_CLEAR);
         if (!recorded)
            return;
         recorded->image_transfer = transfer;
      }
   }
}

static bool
rd_image_transfer_memory(struct rd_image *image, struct rd_buffer *buffer,
                         struct rd_memory **memory, VkDeviceSize *offset, VkDeviceSize *size)
{
   if (image) {
      *memory = image->memory;
      *offset = image->memory_offset;
      *size = image->size;
   } else if (buffer) {
      *memory = buffer->memory;
      *offset = buffer->offset;
      *size = buffer->vk.size;
   } else {
      return false;
   }
   return *memory && *offset <= (*memory)->vk.size && *size <= (*memory)->vk.size - *offset;
}

static bool
rd_image_transfer_bounds(VkDeviceSize offset, VkDeviceSize row, VkDeviceSize slice,
                         const struct rd_image_transfer *transfer, VkDeviceSize size,
                         uint32_t step, uint32_t lane)
{
   if (!transfer->rows || !transfer->slices || !transfer->row_bytes || offset > size)
      return false;
   uint64_t remaining = size - offset;
   uint64_t span = transfer->lane_step ?
      (transfer->row_bytes - 1) * step + lane + 1 : transfer->row_bytes;
   if (span > remaining ||
       (transfer->rows > 1 && row > (remaining - span) / (transfer->rows - 1)))
      return false;
   uint64_t rows = row * (transfer->rows - 1) + span;
   return transfer->slices == 1 || slice <= (remaining - rows) / (transfer->slices - 1);
}

bool
rd_image_copy_gpu_supported(struct rd_device *device,
                            const struct rd_image_transfer *transfer, bool small_copies)
{
   if (!debug_get_option_gpu_image_copy() ||
       fd_device_version(device->fd) < FD_VERSION_SOFTPIN ||
       !transfer->src_buffer || !transfer->dst_image ||
       transfer->src_image || transfer->dst_buffer || transfer->copy_mask || transfer->lane_step ||
       transfer->dst_image->vk.image_type != VK_IMAGE_TYPE_2D ||
       transfer->dst_image->vk.samples != VK_SAMPLE_COUNT_1_BIT ||
       transfer->dst_image->native_layout.tile_mode ||
       transfer->rows > 65535 || transfer->slices > 65535)
      return false;
   VkFormat format = transfer->dst_image->vk.format;
   if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB &&
       format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_B8G8R8A8_SRGB)
      return false;
   struct rd_memory *src, *dst;
   VkDeviceSize src_offset, dst_offset, src_size, dst_size;
   if (!rd_image_transfer_memory(NULL, transfer->src_buffer, &src, &src_offset, &src_size) ||
       !rd_image_transfer_memory(transfer->dst_image, NULL, &dst, &dst_offset, &dst_size) ||
       !rd_image_transfer_bounds(transfer->src_offset, transfer->src_row_pitch,
                                 transfer->src_slice_pitch, transfer, src_size, 0, 0) ||
       !rd_image_transfer_bounds(transfer->dst_offset, transfer->dst_row_pitch,
                                 transfer->dst_slice_pitch, transfer, dst_size, 0, 0) ||
       src->bo == dst->bo || !src->bo || !dst->bo ||
       (!small_copies && transfer->row_bytes * transfer->rows * transfer->slices < 512 * 1024))
      return false;
   VkDeviceSize alignment = (src_offset + transfer->src_offset) |
      (dst_offset + transfer->dst_offset) | transfer->row_bytes |
      transfer->src_row_pitch | transfer->dst_row_pitch |
      transfer->src_slice_pitch | transfer->dst_slice_pitch;
   return !(alignment & 3) && transfer->row_bytes <= UINT32_MAX &&
      transfer->src_row_pitch <= UINT32_MAX && transfer->dst_row_pitch <= UINT32_MAX &&
      transfer->src_slice_pitch <= UINT32_MAX && transfer->dst_slice_pitch <= UINT32_MAX;
}

static void
rd_image_transfer_cache(struct rd_memory *memory, void *map, VkDeviceSize offset,
                        const struct rd_image_transfer *transfer, bool dst, bool flush)
{
   bool ranges = debug_get_option_image_transfer_cache_ranges();
   uint64_t bytes = 0, count = 0;
   if (!ranges) {
      if (flush)
         util_flush_range(map, memory->vk.size);
      else
         util_flush_inval_range(map, memory->vk.size);
      bytes = memory->vk.size;
      count = 1;
   } else {
      VkDeviceSize row = dst ? transfer->dst_row_pitch : transfer->src_row_pitch;
      VkDeviceSize slice = dst ? transfer->dst_slice_pitch : transfer->src_slice_pitch;
      offset += dst ? transfer->dst_offset : transfer->src_offset;
      VkDeviceSize span = transfer->row_bytes;
      if (transfer->lane_step) {
         offset += dst ? transfer->dst_lane : transfer->src_lane;
         span = (span - 1) * (dst ? transfer->dst_step : transfer->src_step) + 1;
      }
      VkDeviceSize atom = util_cache_granularity();
      VkDeviceSize begin = UINT64_MAX, end = 0;
      void (*operation)(void *, size_t) = flush ? util_flush_range_no_fence :
                                                  util_flush_inval_range_no_fence;
      util_pre_flush_fence();
      for (uint32_t z = 0; z < transfer->slices; z++) {
         for (uint32_t y = 0; y < transfer->rows; y++) {
            VkDeviceSize at = offset + z * slice + y * row;
            VkDeviceSize first = at & ~(atom - 1);
            VkDeviceSize last = MIN2(align64(at + span, atom), fd_bo_size(memory->bo));
            if (begin != UINT64_MAX && first > end) {
               operation((uint8_t *)map + begin, end - begin);
               bytes += end - begin;
               count++;
               begin = UINT64_MAX;
               end = 0;
            }
            begin = MIN2(begin, first);
            end = MAX2(end, last);
         }
      }
      if (begin != UINT64_MAX) {
         operation((uint8_t *)map + begin, end - begin);
         bytes += end - begin;
         count++;
      }
      if (flush)
         util_post_flush_fence();
      else
         util_post_flush_inval_fence();
   }
   if (debug_get_option_image_transfer_cache_debug())
      fprintf(stderr, "RADISH_IMAGE_TRANSFER_CACHE op=%s path=%s ranges=%" PRIu64
              " bytes=%" PRIu64 " allocation=%" PRIu64 "\n",
              flush ? "flush" : "invalidate", ranges ? "ranges" : "full",
              count, bytes, (uint64_t)memory->vk.size);
}

static uint64_t
rd_image_clear_cpu(uint8_t *map, const struct rd_image_transfer *transfer)
{
   VkDeviceSize span = transfer->row_bytes;
   uint32_t rows = transfer->rows, slices = transfer->slices;
   if (transfer->dst_row_pitch == span) {
      span *= rows;
      rows = 1;
   }
   if (rows == 1 && transfer->dst_slice_pitch == span) {
      span *= slices;
      slices = 1;
   }
   bool uniform = true;
   for (unsigned i = 1; i < transfer->texel_size; i++)
      uniform &= transfer->clear[i] == transfer->clear[0];
   _Alignas(16) uint8_t block[1024];
   unsigned block_bytes = MIN2(span, sizeof(block));
   block_bytes -= block_bytes % transfer->texel_size;
   const uint8_t *pattern = transfer->clear;
   if (!uniform && block_bytes > transfer->texel_size) {
      memcpy(block, pattern, transfer->texel_size);
      for (unsigned done = transfer->texel_size; done < block_bytes;) {
         unsigned bytes = MIN2(done, block_bytes - done);
         memcpy(block + done, block, bytes);
         done += bytes;
      }
      pattern = block;
   }
   for (uint32_t z = 0; z < slices; z++) {
      for (uint32_t y = 0; y < rows; y++) {
         uint8_t *to = map + transfer->dst_offset + z * transfer->dst_slice_pitch +
                       y * transfer->dst_row_pitch;
         if (uniform) {
            memset(to, pattern[0], span);
         } else {
            for (VkDeviceSize done = 0; done < span;) {
               unsigned bytes = MIN2(span - done, block_bytes);
               memcpy(to + done, pattern, bytes);
               done += bytes;
            }
         }
      }
   }
   return (uint64_t)rows * slices * (uniform ? 1 : DIV_ROUND_UP(span, block_bytes));
}

static uint64_t
rd_image_copy_cpu(uint8_t *dst, const uint8_t *src,
                   const struct rd_image_transfer *transfer, bool coalesce)
{
   VkDeviceSize span = transfer->row_bytes;
   uint32_t rows = transfer->rows, slices = transfer->slices;
   if (coalesce && transfer->src_row_pitch == span && transfer->dst_row_pitch == span) {
      span *= rows;
      rows = 1;
   }
   if (coalesce && rows == 1 && transfer->src_slice_pitch == span &&
       transfer->dst_slice_pitch == span) {
      span *= slices;
      slices = 1;
   }
   for (uint32_t z = 0; z < slices; z++) {
      for (uint32_t y = 0; y < rows; y++) {
         memmove(dst + transfer->dst_offset + z * transfer->dst_slice_pitch +
                    y * transfer->dst_row_pitch,
                 src + transfer->src_offset + z * transfer->src_slice_pitch +
                    y * transfer->src_row_pitch, span);
      }
   }
   return (uint64_t)rows * slices;
}

VkResult
rd_execute_image_transfer(struct rd_device *device, const struct rd_image_transfer *transfer,
                           bool clear, struct rd_submit_window *window)
{
   struct rd_memory *src = NULL, *dst = NULL;
   VkDeviceSize src_offset = 0, dst_offset = 0, src_size = 0, dst_size = 0;
   if (!rd_image_transfer_memory(transfer->dst_image, transfer->dst_buffer, &dst, &dst_offset, &dst_size) ||
       !rd_image_transfer_bounds(transfer->dst_offset, transfer->dst_row_pitch,
                                 transfer->dst_slice_pitch, transfer, dst_size,
                                 transfer->dst_step, transfer->dst_lane))
      return VK_ERROR_UNKNOWN;
   if (!clear && (!rd_image_transfer_memory(transfer->src_image, transfer->src_buffer, &src, &src_offset, &src_size) ||
       !rd_image_transfer_bounds(transfer->src_offset, transfer->src_row_pitch,
                                 transfer->src_slice_pitch, transfer, src_size,
                                 transfer->src_step, transfer->src_lane)))
      return VK_ERROR_UNKNOWN;
   if (clear && (!transfer->texel_size || transfer->texel_size > sizeof(transfer->clear) ||
                 transfer->row_bytes % transfer->texel_size))
      return VK_ERROR_UNKNOWN;
   if (transfer->copy_mask && (clear || transfer->row_bytes % 4 || transfer->lane_step))
      return VK_ERROR_UNKNOWN;
   if (transfer->lane_step && (clear || !transfer->src_step || !transfer->dst_step))
      return VK_ERROR_UNKNOWN;
   if (!clear && rd_image_copy_gpu_supported(device, transfer, false))
      return rd_execute_gpu_image_copy(device, transfer, src, dst,
         src_offset + transfer->src_offset, dst_offset + transfer->dst_offset, window);
   if (src && src->bo != dst->bo && fd_bo_cpu_prep(src->bo, device->pipe, FD_BO_PREP_READ | FD_BO_PREP_NOSYNC))
      return VK_ERROR_DEVICE_LOST;
   uint32_t flags = FD_BO_PREP_WRITE | FD_BO_PREP_NOSYNC;
   if ((src && src->bo == dst->bo) || transfer->copy_mask || transfer->lane_step || dst->cached)
      flags |= FD_BO_PREP_READ;
   if (fd_bo_cpu_prep(dst->bo, device->pipe, flags))
      return VK_ERROR_DEVICE_LOST;
   uint8_t *dst_map = fd_bo_map(dst->bo);
   const uint8_t *src_map = src ? fd_bo_map(src->bo) : NULL;
   if (!dst_map || (src && !src_map))
      return VK_ERROR_MEMORY_MAP_FAILED;
   if (src && src->cached)
      rd_image_transfer_cache(src, (void *)src_map, src_offset, transfer, false, false);
   if (dst->cached)
      rd_image_transfer_cache(dst, dst_map, dst_offset, transfer, true, false);
   __sync_synchronize();
   bool bulk_clear = clear && debug_get_option_cpu_image_clear_bulk();
   uint64_t clear_calls = bulk_clear ? rd_image_clear_cpu(dst_map + dst_offset, transfer) :
      clear ? transfer->row_bytes / transfer->texel_size * transfer->rows * transfer->slices : 0;
   bool plain_copy = !clear && !transfer->copy_mask && !transfer->lane_step;
   if (plain_copy) {
      uint64_t calls = rd_image_copy_cpu(dst_map + dst_offset, src_map + src_offset,
         transfer, src->bo != dst->bo && debug_get_option_cpu_image_copy_bulk());
      if (debug_get_option_cpu_image_copy_debug())
         fprintf(stderr, "RADISH_CPU_IMAGE_COPY bytes=%" PRIu64 " calls=%" PRIu64 "\n",
                 transfer->row_bytes * transfer->rows * transfer->slices, calls);
   }
   if (!bulk_clear && !plain_copy) {
      for (uint32_t z = 0; z < transfer->slices; z++) {
         for (uint32_t y = 0; y < transfer->rows; y++) {
            uint8_t *to = dst_map + dst_offset + transfer->dst_offset +
                           z * transfer->dst_slice_pitch + y * transfer->dst_row_pitch;
            if (clear) {
               for (VkDeviceSize x = 0; x < transfer->row_bytes; x += transfer->texel_size)
                  memcpy(to + x, transfer->clear, transfer->texel_size);
            } else {
               const uint8_t *from = src_map + src_offset + transfer->src_offset +
                                      z * transfer->src_slice_pitch + y * transfer->src_row_pitch;
               if (transfer->lane_step) {
                  for (VkDeviceSize x = 0; x < transfer->row_bytes; x++)
                     to[x * transfer->dst_step + transfer->dst_lane] =
                         from[x * transfer->src_step + transfer->src_lane];
               } else if (transfer->copy_mask) {
                  for (VkDeviceSize x = 0; x < transfer->row_bytes; x += 4) {
                     uint32_t source, target;
                     memcpy(&source, from + x, 4);
                     memcpy(&target, to + x, 4);
                     target = (target & ~transfer->copy_mask) | (source & transfer->copy_mask);
                     memcpy(to + x, &target, 4);
                  }
               } else {
                  memmove(to, from, transfer->row_bytes);
               }
            }
         }
      }
   }
   if (clear && debug_get_option_cpu_image_clear_debug())
      fprintf(stderr, "RADISH_CPU_IMAGE_CLEAR path=%s bytes=%" PRIu64 " texel=%u calls=%" PRIu64 "\n",
              bulk_clear ? "bulk" : "texel", transfer->row_bytes * transfer->rows * transfer->slices,
              transfer->texel_size, clear_calls);
   if (dst->cached)
      rd_image_transfer_cache(dst, dst_map, dst_offset, transfer, true, true);
   __sync_synchronize();
   return VK_SUCCESS;
}
