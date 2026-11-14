#include "rd_private.h"

#include "util/os_misc.h"
#include "util/build_id.h"
#include "freedreno_uuid.h"
#include "util/u_memory.h"
#include "util/cache_ops.h"
#include "drm/msm/msm_common.h"
#include "vk_util.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <xf86drm.h>

VKAPI_ATTR void VKAPI_CALL
rd_GetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice handle,
   const VkPhysicalDeviceExternalBufferInfo *info,
   VkExternalBufferProperties *properties)
{
   VkExternalMemoryProperties memory = {0};
   switch (info->handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
      memory.externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                                      VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
      memory.exportFromImportedHandleTypes =
         VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
         VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      memory.compatibleHandleTypes = memory.exportFromImportedHandleTypes;
      break;
   default:
      break;
   }
   properties->externalMemoryProperties = memory;
}

VKAPI_ATTR void VKAPI_CALL
rd_GetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice handle,
   const VkPhysicalDeviceExternalFenceInfo *info,
   VkExternalFenceProperties *properties)
{
   properties->exportFromImportedHandleTypes = 0;
   properties->compatibleHandleTypes = 0;
   properties->externalFenceFeatures = 0;
}

VKAPI_ATTR void VKAPI_CALL
rd_GetPhysicalDeviceExternalSemaphorePropertiesKHR(VkPhysicalDevice handle,
   const VkPhysicalDeviceExternalSemaphoreInfo *info,
   VkExternalSemaphoreProperties *properties)
{
   properties->exportFromImportedHandleTypes = 0;
   properties->compatibleHandleTypes = 0;
   properties->externalSemaphoreFeatures = 0;

   VK_FROM_HANDLE(rd_physical_device, physical, handle);
   const VkSemaphoreTypeCreateInfo *type_info =
      vk_find_struct_const(info->pNext, SEMAPHORE_TYPE_CREATE_INFO);
   if (info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT &&
       (!type_info || type_info->semaphoreType == VK_SEMAPHORE_TYPE_BINARY) &&
       physical->sync_type.import_sync_file &&
       physical->sync_type.export_sync_file) {
      properties->exportFromImportedHandleTypes =
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
      properties->compatibleHandleTypes =
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
      properties->externalSemaphoreFeatures =
         VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
         VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
   }
}

static const struct vk_instance_extension_table rd_instance_extensions = {
   .KHR_surface = true,
   .KHR_get_surface_capabilities2 = true,
   .EXT_headless_surface = true,
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
   .KHR_wayland_surface = true,
#endif
   .KHR_get_physical_device_properties2 = true,
   .KHR_device_group_creation = true,
   .KHR_external_memory_capabilities = true,
   .KHR_external_fence_capabilities = true,
   .KHR_external_semaphore_capabilities = true,
};

static const struct vk_device_extension_table rd_device_extensions = {
   .KHR_swapchain = true,
   .KHR_incremental_present = true,
   .KHR_device_group = true,
   .KHR_descriptor_update_template = true,
   .KHR_maintenance1 = true,
   .KHR_maintenance3 = true,
   .KHR_sampler_ycbcr_conversion = true,
   .KHR_bind_memory2 = true,
   .KHR_get_memory_requirements2 = true,
   .KHR_external_memory = true,
   .KHR_external_memory_fd = true,
   .KHR_external_semaphore = true,
   .KHR_external_semaphore_fd = true,
   .EXT_external_memory_dma_buf = true,
   .EXT_queue_family_foreign = true,
   .KHR_synchronization2 = true,
   .KHR_copy_commands2 = true,
   .KHR_image_format_list = true,
   .EXT_image_drm_format_modifier = true,
   .KHR_storage_buffer_storage_class = true,
   .KHR_16bit_storage = true,
   .KHR_8bit_storage = true,
   .KHR_shader_float16_int8 = true,
   .KHR_relaxed_block_layout = true,
   .EXT_physical_device_drm = true,
   .KHR_sampler_mirror_clamp_to_edge = true,
   .EXT_host_query_reset = true,
};

VKAPI_ATTR VkResult VKAPI_CALL
rd_EnumerateInstanceVersion(uint32_t *version)
{
   *version = VK_API_VERSION_1_1;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
rd_EnumerateInstanceExtensionProperties(const char *layer, uint32_t *count,
                                      VkExtensionProperties *properties)
{
   if (layer)
      return VK_ERROR_LAYER_NOT_PRESENT;
   return vk_enumerate_instance_extension_properties(&rd_instance_extensions,
                                                     count, properties);
}

static void
rd_destroy_physical(struct vk_physical_device *base)
{
   struct rd_physical_device *physical = container_of(base, struct rd_physical_device, vk);
   rd_wsi_finish(physical);
   fd_device_del(physical->fd);
   close(physical->render_fd);
   vk_physical_device_finish(base);
   vk_free(&base->instance->alloc, physical);
}

static VkResult
rd_try_create_physical(struct vk_instance *instance, drmDevicePtr drm,
                       struct vk_physical_device **out)
{
   if (!(drm->available_nodes & (1 << DRM_NODE_RENDER)))
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   int render_fd = open(drm->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
   if (render_fd < 0)
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   drmVersionPtr version = drmGetVersion(render_fd);
   bool is_msm = version && !strcmp(version->name, "msm");
   drmFreeVersion(version);
   if (!is_msm) {
      close(render_fd);
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }
   struct fd_device *fd = fd_device_new_dup(render_fd);
   if (!fd) {
      close(render_fd);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   struct fd_pipe *pipe = fd_pipe_new(fd, FD_PIPE_3D);
   if (!pipe) {
      fd_device_del(fd);
      close(render_fd);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   uint64_t gpu_id = 0;
   int ret = fd_pipe_get_param(pipe, FD_GPU_ID, &gpu_id);
   const struct fd_dev_id dev_id = *fd_pipe_dev_id(pipe);
   fd_pipe_del(pipe);
   if (ret || gpu_id != 506) {
      fd_device_del(fd);
      close(render_fd);
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }
   struct stat render_stat;
   if (fstat(render_fd, &render_stat) != 0) {
      fd_device_del(fd);
      close(render_fd);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   struct stat primary_stat;
   bool has_primary = (drm->available_nodes & (1 << DRM_NODE_PRIMARY)) &&
      drm->nodes[DRM_NODE_PRIMARY] &&
      stat(drm->nodes[DRM_NODE_PRIMARY], &primary_stat) == 0;
   struct rd_physical_device *physical = vk_zalloc(&instance->alloc,
      sizeof(*physical), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!physical) {
      fd_device_del(fd);
      close(render_fd);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   struct vk_physical_device_dispatch_table dispatch;
   vk_physical_device_dispatch_table_from_entrypoints(&dispatch,
      &rd_physical_device_entrypoints, true);
   vk_physical_device_dispatch_table_from_entrypoints(&dispatch,
      &wsi_physical_device_entrypoints, false);
   struct vk_properties properties = {
      .apiVersion = VK_API_VERSION_1_1,
      .driverVersion = 1,
      .vendorID = 0x5143,
      .deviceID = 506,
      .deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
      .subgroupSize = 16,
      .subgroupSupportedStages = VK_SHADER_STAGE_COMPUTE_BIT,
      .subgroupSupportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT,
      .maxMemoryAllocationCount = 4096,
      .bufferImageGranularity = 64,
      .discreteQueuePriorities = 2,
      .optimalBufferCopyOffsetAlignment = 1,
      .optimalBufferCopyRowPitchAlignment = 1,
      .maxPerSetDescriptors = 1024,
      .maxMemoryAllocationSize = UINT32_MAX - 4095ull,
      .maxImageDimension1D = 4096,
      .maxImageDimension2D = RD_IMAGE_MAX_2D_SIZE,
      .maxImageDimension3D = 256,
      .maxImageDimensionCube = 4096,
      .maxImageArrayLayers = 256,
      .minTexelBufferOffsetAlignment = 64,
      .maxTexelBufferElements = 1 << 27,
      .maxSamplerAllocationCount = 4096,
      .maxSamplerLodBias = 16.0f,
      .maxSamplerAnisotropy = 1.0f,
      .pointSizeRange = {1.0f, 1.0f},
      .lineWidthRange = {1.0f, 1.0f},
      .maxViewports = 1,
      .maxViewportDimensions = {4096, 4096},
      .viewportBoundsRange = {-8192.0f, 8191.0f},
      .maxFramebufferWidth = 4096,
      .maxFramebufferHeight = 4096,
      .maxFramebufferLayers = 256,
      .framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,
      .framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,
      .framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,
      .framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,
      .maxColorAttachments = 4,
      .maxVertexInputAttributes = 16,
      .maxVertexInputBindings = 16,
      .maxVertexInputAttributeOffset = 2047,
      .maxVertexInputBindingStride = 2048,
      .maxVertexOutputComponents = 64,
      .maxFragmentInputComponents = 64,
      .maxFragmentOutputAttachments = 4,
      .maxFragmentCombinedOutputResources = 4,
      .maxDrawIndirectCount = 1,
      .maxDrawIndexedIndexValue = (1u << 24) - 1,
      .minTexelOffset = -8,
      .maxTexelOffset = 7,
      .subPixelPrecisionBits = 4,
      .subTexelPrecisionBits = 4,
      .mipmapPrecisionBits = 8,
      .sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,
      .sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,
      .sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,
      .sampledImageStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,
      .storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT,
      .maxSampleMaskWords = 1,
      .maxBoundDescriptorSets = 4,
      .maxPerStageDescriptorSamplers = 16,
      .maxPerStageDescriptorUniformBuffers = 12,
      .maxPerStageDescriptorStorageBuffers = 5,
      .maxPerStageDescriptorSampledImages = 16,
      .maxPerStageDescriptorStorageImages = 4,
      .maxPerStageDescriptorInputAttachments = 4,
      .maxPerStageResources = 128,
      .maxDescriptorSetSamplers = 96,
      .maxDescriptorSetUniformBuffers = 72,
      .maxDescriptorSetUniformBuffersDynamic = 8,
      .maxDescriptorSetStorageBuffers = 24,
      .maxDescriptorSetStorageBuffersDynamic = 4,
      .maxDescriptorSetSampledImages = 96,
      .maxDescriptorSetStorageImages = 24,
      .maxDescriptorSetInputAttachments = 4,
      .timestampComputeAndGraphics = true,
      .timestampPeriod = 1000000000.0f / 19200000.0f,
      .drmHasPrimary = has_primary,
      .drmHasRender = true,
      .drmPrimaryMajor = has_primary ? major(primary_stat.st_rdev) : 0,
      .drmPrimaryMinor = has_primary ? minor(primary_stat.st_rdev) : 0,
      .drmRenderMajor = major(render_stat.st_rdev),
      .drmRenderMinor = minor(render_stat.st_rdev),
      .maxStorageBufferRange = 128 * 1024 * 1024,
      .maxUniformBufferRange = 16384,
      .maxPushConstantsSize = RD_MAX_PUSH_CONSTANTS_SIZE,
      .minMemoryMapAlignment = 64,
      .minStorageBufferOffsetAlignment = 16,
      .minUniformBufferOffsetAlignment = 16,
      .nonCoherentAtomSize = 64,
      .maxComputeSharedMemorySize = RD_MAX_SHARED_SIZE,
      .maxComputeWorkGroupCount = {65535, 65535, 65535},
      .maxComputeWorkGroupInvocations = RD_MAX_WORKGROUP_INVOCATIONS,
      .maxComputeWorkGroupSize = {RD_MAX_WORKGROUP_INVOCATIONS, RD_MAX_WORKGROUP_INVOCATIONS, 64},
      /* multiview: renders replay once per view */
      .maxMultiviewViewCount = 6,
      .maxMultiviewInstanceIndex = (1u << 27) - 1,
   };
   fd_get_device_uuid(properties.deviceUUID, &dev_id);
   fd_get_driver_uuid(properties.driverUUID);
   const struct build_id_note *build_id = build_id_find_nhdr_for_addr(rd_try_create_physical);
   if (build_id && build_id_length(build_id) >= VK_UUID_SIZE)
      memcpy(properties.driverUUID, build_id_data(build_id), VK_UUID_SIZE);
   memcpy(properties.pipelineCacheUUID, properties.driverUUID, VK_UUID_SIZE);
   properties.deviceNodeMask = 1;
   snprintf(properties.deviceName, sizeof(properties.deviceName),
            "Radish Adreno 506 (experimental compute)");
   size_t cache_granularity = util_cache_granularity();
   physical->has_cached_memory = cache_granularity &&
      msm_common_is_memory_type_supported(fd_device_fd(fd), sysconf(_SC_PAGESIZE), MSM_BO_CACHED);
   if (physical->has_cached_memory)
      properties.nonCoherentAtomSize = MAX2(properties.nonCoherentAtomSize, cache_granularity);
   physical->sync_type = vk_drm_syncobj_get_type(render_fd);
   const bool sync_file = physical->sync_type.import_sync_file &&
                          physical->sync_type.export_sync_file;
   const uint32_t timeline_sync_features = VK_SYNC_FEATURE_TIMELINE |
      VK_SYNC_FEATURE_CPU_WAIT | VK_SYNC_FEATURE_CPU_SIGNAL |
      VK_SYNC_FEATURE_WAIT_ANY | VK_SYNC_FEATURE_GPU_WAIT;
   const bool timeline =
      (physical->sync_type.features & timeline_sync_features) == timeline_sync_features &&
      physical->sync_type.get_value && physical->sync_type.signal &&
      physical->sync_type.wait_many;
   struct vk_device_extension_table extensions = rd_device_extensions;
   extensions.KHR_external_semaphore = sync_file;
   extensions.KHR_external_semaphore_fd = sync_file;
   extensions.KHR_timeline_semaphore = timeline;
   properties.maxTimelineSemaphoreValueDifference = timeline ? UINT64_MAX : 0;
   const struct vk_features features = { .pipelineStatisticsQuery = true, .robustBufferAccess = true,
      .samplerYcbcrConversion = true,
      .synchronization2 = true,
      .multiview = true, .occlusionQueryPrecise = true, .hostQueryReset = true,
      .textureCompressionETC2 = true,
      .storageBuffer16BitAccess = true, .storageBuffer8BitAccess = true, .shaderInt8 = true, .timelineSemaphore = timeline };
   VkResult result = vk_physical_device_init(&physical->vk, instance, &extensions,
                                             &features, &properties, &dispatch);
   if (result != VK_SUCCESS) {
      vk_free(&instance->alloc, physical);
      fd_device_del(fd);
      close(render_fd);
      return result;
   }
   physical->fd = fd;
   physical->render_fd = render_fd;
   physical->sync_wait_many = physical->sync_type.wait_many;
   physical->sync_type.wait_many = rd_sync_wait_many;
   physical->sync_types[0] = &physical->sync_type;
   physical->vk.supported_sync_types = physical->sync_types;
   result = rd_wsi_init(physical);
   if (result != VK_SUCCESS) {
      rd_destroy_physical(&physical->vk);
      return result;
   }
   *out = &physical->vk;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
rd_CreateInstance(const VkInstanceCreateInfo *info,
                  const VkAllocationCallbacks *allocator, VkInstance *out)
{
   if (!allocator)
      allocator = vk_default_allocator();
   struct rd_instance *instance = vk_zalloc(allocator, sizeof(*instance), 8,
                                           VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct vk_instance_dispatch_table dispatch;
   vk_instance_dispatch_table_from_entrypoints(&dispatch,
                                               &rd_instance_entrypoints, true);
   vk_instance_dispatch_table_from_entrypoints(&dispatch,
                                               &wsi_instance_entrypoints, false);
   VkResult result = vk_instance_init(&instance->vk, &rd_instance_extensions,
                                      &dispatch, info, allocator);
   if (result != VK_SUCCESS) {
      vk_free(allocator, instance);
      return result;
   }
   instance->vk.physical_devices.try_create_for_drm = rd_try_create_physical;
   instance->vk.physical_devices.destroy = rd_destroy_physical;
   *out = rd_instance_to_handle(instance);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
rd_DestroyInstance(VkInstance handle, const VkAllocationCallbacks *allocator)
{
   VK_FROM_HANDLE(rd_instance, instance, handle);
   if (!instance)
      return;
   vk_instance_finish(&instance->vk);
   vk_free(&instance->vk.alloc, instance);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
rd_GetInstanceProcAddr(VkInstance handle, const char *name)
{
   VK_FROM_HANDLE(rd_instance, instance, handle);
   return vk_instance_get_proc_addr(instance ? &instance->vk : NULL,
                                    &rd_instance_entrypoints, name);
}

PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name)
{
   return rd_GetInstanceProcAddr(instance, name);
}

VKAPI_ATTR void VKAPI_CALL
rd_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice handle,
   uint32_t *count, VkQueueFamilyProperties2 *properties)
{
   VK_OUTARRAY_MAKE_TYPED(VkQueueFamilyProperties2, out, properties, count);
   vk_outarray_append_typed(VkQueueFamilyProperties2, &out, p) {
      p->queueFamilyProperties = (VkQueueFamilyProperties) {
         .queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT |
                       (rd_graphics_enabled() ? VK_QUEUE_GRAPHICS_BIT : 0),
         .queueCount = 1,
         .timestampValidBits = 64,
         .minImageTransferGranularity = {1, 1, 1},
      };
   }
}

VKAPI_ATTR void VKAPI_CALL
rd_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice handle,
                                     VkPhysicalDeviceMemoryProperties2 *out)
{
   VK_FROM_HANDLE(rd_physical_device, physical, handle);
   uint64_t total = 0;
   os_get_total_physical_memory(&total);
   out->memoryProperties = (VkPhysicalDeviceMemoryProperties) {
      .memoryTypeCount = 1,
      .memoryTypes = {{ .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, .heapIndex = 0 }},
      .memoryHeapCount = 1,
      .memoryHeaps = {{ .size = total / 2, .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT }},
   };
   if (physical->has_cached_memory) {
      out->memoryProperties.memoryTypeCount = 2;
      out->memoryProperties.memoryTypes[1] = (VkMemoryType) {
         .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
         .heapIndex = 0,
      };
   }
}

VKAPI_ATTR void VKAPI_CALL
rd_GetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice handle,
   const VkPhysicalDeviceSparseImageFormatInfo2 *info, uint32_t *count,
   VkSparseImageFormatProperties2 *properties)
{
   *count = 0;
}
