#include "rd_private.h"
#include "wsi_common_drm.h"
#include "util/u_math.h"

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
rd_wsi_proc_addr(VkPhysicalDevice handle, const char *name)
{
   VK_FROM_HANDLE(rd_physical_device, physical, handle);
   return vk_instance_get_proc_addr_unchecked(physical->vk.instance, name);
}

static bool
rd_wsi_can_present_on_device(VkPhysicalDevice handle, int fd)
{
   VK_FROM_HANDLE(rd_physical_device, physical, handle);
   return wsi_common_drm_devices_equal(fd, physical->render_fd);
}

VkResult
rd_wsi_init(struct rd_physical_device *physical)
{
   const struct wsi_device_options options = { .sw_device = false };
   VkResult result = wsi_device_init(&physical->wsi,
      rd_physical_device_to_handle(physical), rd_wsi_proc_addr,
      &physical->vk.instance->alloc, -1, NULL, &options);
   if (result != VK_SUCCESS)
      return result;

   physical->wsi.supports_scanout = false;
   physical->wsi.supports_modifiers = true;
   physical->wsi.wants_linear = true;
   physical->wsi.force_bgra8_unorm_first = true;
   physical->wsi.can_present_on_device = rd_wsi_can_present_on_device;
   physical->wsi.maxImageDimension2D = MIN2(
      physical->vk.properties.maxImageDimension2D,
      MIN2(physical->vk.properties.maxFramebufferWidth,
           physical->vk.properties.maxFramebufferHeight));
   physical->vk.wsi_device = &physical->wsi;
   return VK_SUCCESS;
}

void
rd_wsi_finish(struct rd_physical_device *physical)
{
   if (!physical->vk.wsi_device)
      return;
   physical->vk.wsi_device = NULL;
   wsi_device_finish(&physical->wsi, &physical->vk.instance->alloc);
}
