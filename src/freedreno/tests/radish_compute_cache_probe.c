#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define N 4096u
#define CHECK(call) do { VkResult r = (call); if (r != VK_SUCCESS) { \
   fprintf(stderr, "%s: %d\n", #call, r); exit(1); } } while (0)
static uint64_t now(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec*1000000000+ts.tv_nsec;
}
int main(int argc, char **argv)
{
   if (argc != 2) return 2;
   FILE *file = fopen(argv[1], "rb");
   if (!file) return 2;
   fseek(file, 0, SEEK_END); long length = ftell(file); rewind(file);
   uint32_t *code = malloc(length);
   if (!code || fread(code, 1, length, file) != (size_t)length) return 2;
   fclose(file);
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app };
   VkInstance instance;
   CHECK(vkCreateInstance(&ii, NULL, &instance));
   VkPhysicalDevice physical; uint32_t count = 1;
   CHECK(vkEnumeratePhysicalDevices(instance, &count, &physical));
   float priority = 1;
   VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority };
   VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi };
   VkDevice device;
   CHECK(vkCreateDevice(physical, &di, NULL, &device));
   VkQueue queue; vkGetDeviceQueue(device, 0, 0, &queue);
   VkShaderModuleCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = length, .pCode = code };
   VkShaderModule shader;
   CHECK(vkCreateShaderModule(device, &si, NULL, &shader)); free(code);
   VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = N*4, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
   VkBuffer buffer; CHECK(vkCreateBuffer(device, &bi, NULL, &buffer));
   VkMemoryRequirements req; vkGetBufferMemoryRequirements(device, buffer, &req);
   VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size, .memoryTypeIndex = 0 };
   VkDeviceMemory memory; CHECK(vkAllocateMemory(device, &mai, NULL, &memory));
   CHECK(vkBindBufferMemory(device, buffer, memory, 0));
   uint32_t *map; CHECK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&map));
   VkDescriptorSetLayoutBinding binding = { .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
   VkDescriptorSetLayoutCreateInfo sli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1, .pBindings = &binding };
   VkDescriptorSetLayout set_layout;
   CHECK(vkCreateDescriptorSetLayout(device, &sli, NULL, &set_layout));
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
   VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
   VkDescriptorPool descriptor_pool;
   CHECK(vkCreateDescriptorPool(device, &dpi, NULL, &descriptor_pool));
   VkDescriptorSetAllocateInfo sai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout };
   VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device, &sai, &set));
   VkDescriptorBufferInfo buffer_info = {buffer,0,N*4};
   VkWriteDescriptorSet write = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set, .descriptorCount = 1, .descriptorType = binding.descriptorType,
      .pBufferInfo = &buffer_info };
   vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
   VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &set_layout };
   VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device, &pli, NULL, &layout));
   VkComputePipelineCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main" }, .layout = layout };
   uint64_t start = now();
   VkPipeline pipeline;
   CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline));
   printf("COMPUTE_PIPELINE_CREATE ms=%.4f\n", (now()-start)/1e6);
   VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
   VkCommandPool pool; CHECK(vkCreateCommandPool(device, &cpi, NULL, &pool));
   VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer command; CHECK(vkAllocateCommandBuffers(device, &cai, &command));
   for (unsigned trial = 0; trial < 3; trial++) {
      memset(map, 0xcd, N*4);
      CHECK(vkResetCommandBuffer(command, 0));
      VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      CHECK(vkBeginCommandBuffer(command, &begin));
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
      vkCmdDispatch(command, N/32, 1, 1);
      VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
      CHECK(vkEndCommandBuffer(command));
      VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1, .pCommandBuffers = &command };
      CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
      CHECK(vkQueueWaitIdle(queue));
      for (unsigned i = 0; i < N; i++) {
         uint32_t neighbor = (i & ~31u) + ((i+13u) & 31u);
         uint32_t expected = (neighbor*1664525u+1013904223u) ^ (i>>3u);
         if (map[i] != expected) {
            fprintf(stderr, "COMPUTE_FAIL trial=%u index=%u got=%u expected=%u\n",
                    trial,i,map[i],expected); return 5;
         }
      }
      printf("COMPUTE_READBACK trial=%u values=%u PASS\n", trial,N);
   }
   vkDestroyCommandPool(device, pool, NULL); vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyPipelineLayout(device, layout, NULL); vkDestroyDescriptorPool(device, descriptor_pool, NULL);
   vkDestroyDescriptorSetLayout(device, set_layout, NULL); vkDestroyShaderModule(device, shader, NULL);
   vkUnmapMemory(device, memory); vkDestroyBuffer(device, buffer, NULL); vkFreeMemory(device, memory, NULL);
   vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
   return 0;
}
