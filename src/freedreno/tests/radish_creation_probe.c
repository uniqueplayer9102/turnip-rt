#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

struct allocation { void *base; size_t size; };
struct accounting { size_t calls, live, fail_at; };
static void release(void *user, void *pointer)
{
   if (!pointer) return;
   struct accounting *state = user;
   struct allocation *header = (struct allocation *)pointer - 1;
   assert(state->live);
   state->live--;
   free(header->base);
}
static void *allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope)
{
   (void)scope;
   struct accounting *state = user;
   if (++state->calls == state->fail_at) return NULL;
   if (alignment < sizeof(void *)) alignment = sizeof(void *);
   void *base = malloc(size + alignment + sizeof(struct allocation));
   if (!base) return NULL;
   uintptr_t aligned = ((uintptr_t)base + sizeof(struct allocation) + alignment - 1) & ~(alignment - 1);
   struct allocation *header = (struct allocation *)aligned - 1;
   *header = (struct allocation){base, size};
   state->live++;
   return (void *)aligned;
}
static void *resize(void *user, void *pointer, size_t size, size_t alignment, VkSystemAllocationScope scope)
{
   if (!size) { release(user, pointer); return NULL; }
   void *copy = allocate(user, size, alignment, scope);
   if (!copy) return NULL;
   if (pointer) {
      struct allocation *header = (struct allocation *)pointer - 1;
      memcpy(copy, pointer, size < header->size ? size : header->size);
      release(user, pointer);
   }
   return copy;
}
static VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
   VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
   (void)severity; (void)type; (void)data; (void)user;
   return VK_FALSE;
}
int main(int argc, char **argv)
{
   if (argc != 2) return 2;
   void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!library) { fprintf(stderr, "%s\n", dlerror()); return 3; }
   PFN_vkGetInstanceProcAddr get = (PFN_vkGetInstanceProcAddr)dlsym(library, "vk_icdGetInstanceProcAddr");
   assert(get);
   PFN_vkCreateInstance create = (PFN_vkCreateInstance)get(VK_NULL_HANDLE, "vkCreateInstance");
   assert(create);
   const char *missing = "VK_FAKE_radish_test_missing";
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "radish-failure-test", .pEngineName = "allocation-accounting", .apiVersion = VK_API_VERSION_1_1};
   VkDebugUtilsMessengerCreateInfoEXT messages[2] = {0};
   for (unsigned i = 0; i < 2; i++) {
      messages[i].sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
      messages[i].messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      messages[i].messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
      messages[i].pfnUserCallback = message;
   }
   messages[0].pNext = &messages[1];
   VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pNext = messages, .pApplicationInfo = &app, .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = &missing};
   for (size_t failure = 0; failure <= 6; failure++) {
      struct accounting state = {.fail_at = failure};
      VkAllocationCallbacks callbacks = {.pUserData = &state, .pfnAllocation = allocate,
         .pfnReallocation = resize, .pfnFree = release};
      VkInstance instance = VK_NULL_HANDLE;
      VkResult result = create(&info, &callbacks, &instance);
      assert(result == VK_ERROR_EXTENSION_NOT_PRESENT || result == VK_ERROR_OUT_OF_HOST_MEMORY);
      assert(!state.live);
   }
   struct accounting state = {0};
   VkAllocationCallbacks callbacks = {.pUserData = &state, .pfnAllocation = allocate,
      .pfnReallocation = resize, .pfnFree = release};
   info.pNext = NULL; info.enabledExtensionCount = 0; info.ppEnabledExtensionNames = NULL;
   VkInstance instance = VK_NULL_HANDLE;
   assert(create(&info, &callbacks, &instance) == VK_SUCCESS);
   PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)get(instance, "vkDestroyInstance");
   PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)get(instance, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceFeatures features = (PFN_vkGetPhysicalDeviceFeatures)get(instance, "vkGetPhysicalDeviceFeatures");
   PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)get(instance, "vkCreateDevice");
   assert(destroy && enumerate && features && create_device);
   uint32_t count = 1;
   VkPhysicalDevice physical;
   assert(enumerate(instance, &count, &physical) == VK_SUCCESS && count == 1);
   VkPhysicalDeviceFeatures supported;
   features(physical, &supported);
   assert(!supported.pipelineStatisticsQuery && supported.robustBufferAccess);
   float priority = 1;
   VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
   VkPhysicalDeviceFeatures requested = {.pipelineStatisticsQuery = VK_TRUE};
   VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue, .pEnabledFeatures = &requested};
   VkDevice device = VK_NULL_HANDLE;
   assert(create_device(physical, &device_info, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT);
   destroy(instance, &callbacks);
   assert(!state.live);
   dlclose(library);
   puts("CREATION PASS: initialization failure/OOM cleanup and accurate statistics feature reporting");
   return 0;
}
