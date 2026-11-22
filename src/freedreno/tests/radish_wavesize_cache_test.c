#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

#define CHECK(call) do { VkResult result = (call); if (result != VK_SUCCESS) { \
   fprintf(stderr, "%s: %d\n", #call, result); exit(1); } } while (0)

int
main(int argc, char **argv)
{
   if (argc != 2)
      return 2;
   FILE *file = fopen(argv[1], "rb");
   if (!file)
      return 2;
   fseek(file, 0, SEEK_END);
   long length = ftell(file);
   rewind(file);
   if (length <= 0 || length % sizeof(uint32_t))
      return 2;
   uint32_t *code = malloc(length);
   if (!code || fread(code, 1, length, file) != (size_t)length)
      return 2;
   fclose(file);
   VkInstanceCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
   VkInstance instance;
   CHECK(vkCreateInstance(&ii, NULL, &instance));
   VkPhysicalDevice physical;
   uint32_t count = 1;
   CHECK(vkEnumeratePhysicalDevices(instance, &count, &physical));
   float priority = 1;
   VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority };
   VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi };
   VkDevice device;
   CHECK(vkCreateDevice(physical, &di, NULL, &device));
   VkShaderModuleCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = length, .pCode = code };
   VkShaderModule shader;
   CHECK(vkCreateShaderModule(device, &si, NULL, &shader));
   free(code);
   VkPipelineLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
   VkPipelineLayout layout;
   CHECK(vkCreatePipelineLayout(device, &li, NULL, &layout));
   VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
   VkPipelineCache cache;
   CHECK(vkCreatePipelineCache(device, &ci, NULL, &cache));
   const char *modes[] = { "single", "double", "any", "single", "invalid", "double", "any" };
   size_t previous = 0;
   for (unsigned pass = 0; pass < 2; pass++) {
      for (unsigned i = 0; i < 7; i++) {
         if (setenv("RADISH_WAVESIZE", modes[i], 1))
            return 2;
         VkComputePipelineCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main" },
            .layout = layout };
         VkPipeline pipeline;
         CHECK(vkCreateComputePipelines(device, cache, 1, &pi, NULL, &pipeline));
         vkDestroyPipeline(device, pipeline, NULL);
         size_t size = 0;
         CHECK(vkGetPipelineCacheData(device, cache, &size, NULL));
         if ((!pass && i < 3 && size <= previous) || ((pass || i >= 3) && size != previous)) {
            fprintf(stderr, "CACHE_KEY_FAIL pass=%u mode=%s size=%zu previous=%zu\n",
                    pass, modes[i], size, previous);
            return 5;
         }
         previous = size;
         printf("CACHE_KEY pass=%u mode=%s bytes=%zu PASS\n", pass, modes[i], size);
      }
      if (!pass) {
         void *data = malloc(previous);
         if (!data)
            return 2;
         size_t size = previous;
         CHECK(vkGetPipelineCacheData(device, cache, &size, data));
         vkDestroyPipelineCache(device, cache, NULL);
         ci.initialDataSize = size;
         ci.pInitialData = data;
         CHECK(vkCreatePipelineCache(device, &ci, NULL, &cache));
         free(data);
      }
   }
   vkDestroyPipelineCache(device, cache, NULL);
   vkDestroyPipelineLayout(device, layout, NULL);
   vkDestroyShaderModule(device, shader, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   return 0;
}
