#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define W 128u
#define H 128u
#define N 128u
#define CHECK(call) do { VkResult r = (call); if (r != VK_SUCCESS) { \
   fprintf(stderr, "%s: %d\n", #call, r); exit(1); } } while (0)
static VkDevice device;
static VkDeviceMemory allocate(VkMemoryRequirements req)
{
   VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size, .memoryTypeIndex = 0 };
   VkDeviceMemory memory;
   CHECK(vkAllocateMemory(device, &ai, NULL, &memory));
   return memory;
}
static VkShaderModule shader(const char *path)
{
   FILE *file = fopen(path, "rb");
   if (!file) exit(2);
   fseek(file, 0, SEEK_END);
   long size = ftell(file);
   rewind(file);
   uint32_t *code = malloc(size);
   if (!code || fread(code, 1, size, file) != (size_t)size) exit(2);
   fclose(file);
   VkShaderModuleCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = size, .pCode = code };
   VkShaderModule module;
   CHECK(vkCreateShaderModule(device, &si, NULL, &module));
   free(code);
   return module;
}
static uint64_t now(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static int compare_double(const void *a, const void *b)
{
   double x = *(const double *)a, y = *(const double *)b;
   return (x > y) - (x < y);
}
int main(int argc, char **argv)
{
   if (argc != 3) return 2;
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app };
   VkInstance instance;
   CHECK(vkCreateInstance(&ii, NULL, &instance));
   uint32_t count = 1;
   VkPhysicalDevice physical;
   CHECK(vkEnumeratePhysicalDevices(instance, &count, &physical));
   VkPhysicalDeviceMultiDrawFeaturesEXT multi_feature = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT };
   VkPhysicalDeviceFeatures2 features = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &multi_feature };
   vkGetPhysicalDeviceFeatures2(physical, &features);
   VkPhysicalDeviceMultiDrawPropertiesEXT multi_properties = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_PROPERTIES_EXT };
   VkPhysicalDeviceProperties2 properties = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &multi_properties };
   vkGetPhysicalDeviceProperties2(physical, &properties);
   if (!multi_feature.multiDraw || multi_properties.maxMultiDrawCount < 1024) {
      fprintf(stderr, "MULTI_DRAW unavailable\n"); return 3;
   }
   printf("DEVICE %s maxMultiDrawCount=%u\n", properties.properties.deviceName,
          multi_properties.maxMultiDrawCount);
   const char *extension = VK_EXT_MULTI_DRAW_EXTENSION_NAME;
   float priority = 1;
   VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority };
   VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &multi_feature, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
      .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension };
   CHECK(vkCreateDevice(physical, &di, NULL, &device));
   PFN_vkCmdDrawMultiEXT multi = (PFN_vkCmdDrawMultiEXT)vkGetDeviceProcAddr(device, "vkCmdDrawMultiEXT");
   PFN_vkCmdDrawMultiIndexedEXT indexed = (PFN_vkCmdDrawMultiIndexedEXT)vkGetDeviceProcAddr(device, "vkCmdDrawMultiIndexedEXT");
   if (!multi || !indexed) return 3;
   VkQueue queue;
   vkGetDeviceQueue(device, 0, 0, &queue);
   VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
      .extent = {W,H,1}, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
   VkImage image;
   CHECK(vkCreateImage(device, &ici, NULL, &image));
   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(device, image, &req);
   VkDeviceMemory image_memory = allocate(req);
   CHECK(vkBindImageMemory(device, image, image_memory, 0));
   VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = ici.format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1} };
   VkImageView view;
   CHECK(vkCreateImageView(device, &vi, NULL, &view));
   VkBuffer buffers[3]; VkDeviceMemory memories[3]; void *maps[3];
   const VkDeviceSize sizes[] = {W*H*4, N*6*4, 16};
   const VkBufferUsageFlags usage[] = {VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
   for (unsigned i = 0; i < 3; i++) {
      VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = sizes[i], .usage = usage[i] };
      CHECK(vkCreateBuffer(device, &bi, NULL, &buffers[i]));
      vkGetBufferMemoryRequirements(device, buffers[i], &req);
      memories[i] = allocate(req);
      CHECK(vkBindBufferMemory(device, buffers[i], memories[i], 0));
      CHECK(vkMapMemory(device, memories[i], 0, VK_WHOLE_SIZE, 0, &maps[i]));
   }
   const float color[] = {1,0,0,1};
   memcpy(maps[2], color, sizeof(color));
   VkDescriptorSetLayoutBinding binding = { .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
   VkDescriptorSetLayoutCreateInfo sli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1, .pBindings = &binding };
   VkDescriptorSetLayout set_layout;
   CHECK(vkCreateDescriptorSetLayout(device, &sli, NULL, &set_layout));
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1};
   VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
   VkDescriptorPool descriptor_pool;
   CHECK(vkCreateDescriptorPool(device, &dpi, NULL, &descriptor_pool));
   VkDescriptorSetAllocateInfo sai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout };
   VkDescriptorSet set;
   CHECK(vkAllocateDescriptorSets(device, &sai, &set));
   VkDescriptorBufferInfo buffer_info = {buffers[2],0,16};
   VkWriteDescriptorSet write = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set, .descriptorCount = 1, .descriptorType = binding.descriptorType,
      .pBufferInfo = &buffer_info };
   vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
   VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &set_layout };
   VkPipelineLayout layout;
   CHECK(vkCreatePipelineLayout(device, &pli, NULL, &layout));
   VkAttachmentDescription attachment = { .format = ici.format, .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
   VkAttachmentReference reference = {0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription subpass = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1, .pColorAttachments = &reference };
   VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass };
   VkRenderPass pass;
   CHECK(vkCreateRenderPass(device, &rpi, NULL, &pass));
   VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = pass, .attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1 };
   VkFramebuffer framebuffer;
   CHECK(vkCreateFramebuffer(device, &fi, NULL, &framebuffer));
   VkShaderModule modules[] = {shader(argv[1]),shader(argv[2])};
   VkPipelineShaderStageCreateInfo stages[] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = modules[0], .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = modules[1], .pName = "main" }};
   VkPipelineVertexInputStateCreateInfo vertex = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
   VkPipelineInputAssemblyStateCreateInfo assembly = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkViewport viewport = {0,0,W,H,0,1}; VkRect2D scissor = {{0,0},{W,H}};
   VkPipelineViewportStateCreateInfo vpi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor };
   VkPipelineRasterizationStateCreateInfo raster = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1 };
   VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   VkPipelineColorBlendAttachmentState blend = { .colorWriteMask = 15 };
   VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &blend };
   VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex,
      .pInputAssemblyState = &assembly, .pViewportState = &vpi, .pRasterizationState = &raster,
      .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = layout, .renderPass = pass };
   VkPipeline pipeline;
   CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, NULL, &pipeline));
   VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 };
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(device, &cpi, NULL, &pool));
   VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer command;
   CHECK(vkAllocateCommandBuffers(device, &cai, &command));
   cai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
   VkCommandBuffer secondary;
   CHECK(vkAllocateCommandBuffers(device, &cai, &secondary));
   VkCommandBufferInheritanceInfo inheritance = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
      .renderPass = pass, .framebuffer = framebuffer };
   VkCommandBufferBeginInfo secondary_begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
      .pInheritanceInfo = &inheritance };
   struct { VkMultiDrawInfoEXT info; uint32_t padding[3]; } draws[N+2] = {0};
   struct { VkMultiDrawIndexedInfoEXT info; uint32_t padding[2]; } indices[N+2] = {0};
   uint32_t compact[N+2][3] = {{0}};
   for (unsigned i = 0; i < N; i++) {
      draws[i+1].info = (VkMultiDrawInfoEXT){i*6,i == 37 ? 0 : 6};
      indices[i+1].info = (VkMultiDrawIndexedInfoEXT){i*6,i == 37 ? 0 : 6,(int32_t)(i%3)*6-6};
      compact[i+1][0] = i*6; compact[i+1][1] = i == 37 ? 0 : 6;
      compact[i+1][2] = 123456;
   }
   VkClearValue clear = { .color = {{0,0,0,1}} };
   VkRenderPassBeginInfo begin_pass = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = pass, .framebuffer = framebuffer, .renderArea = {{0,0},{W,H}},
      .clearValueCount = 1, .pClearValues = &clear };
   VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   uint8_t baseline[W*H*4];
   for (unsigned mode = 0; mode < 7; mode++) {
      unsigned kind = mode < 4 ? mode : mode - 3;
      for (unsigned i = 0; i < N; i++) for (unsigned k = 0; k < 6; k++)
         ((uint32_t *)maps[1])[i*6+k] = i*6+k-(kind == 2 ? indices[i+1].info.vertexOffset : -6);
      CHECK(vkResetCommandBuffer(command, 0));
      CHECK(vkBeginCommandBuffer(command, &begin));
      vkCmdBeginRenderPass(command, &begin_pass, mode < 4 ?
         VK_SUBPASS_CONTENTS_INLINE : VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
      VkCommandBuffer draw_command = mode < 4 ? command : secondary;
      if (mode >= 4) {
         CHECK(vkResetCommandBuffer(secondary, 0));
         CHECK(vkBeginCommandBuffer(secondary, &secondary_begin));
      }
      vkCmdBindPipeline(draw_command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      vkCmdBindDescriptorSets(draw_command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
      vkCmdBindIndexBuffer(draw_command, buffers[1], 0, VK_INDEX_TYPE_UINT32);
      multi(draw_command, 0, NULL, 2, 7, 0);
      indexed(draw_command, 0, NULL, 2, 7, 0, NULL);
      multi(draw_command, N+2, &draws[0].info, 0, 7, sizeof(draws[0]));
      indexed(draw_command, N+2, &indices[0].info, 0, 7, sizeof(indices[0]), NULL);
      if (kind == 0) {
         for (unsigned i = 0; i < N+2; i++)
            vkCmdDraw(draw_command, draws[i].info.vertexCount, 2, draws[i].info.firstVertex, 7);
      } else if (kind == 1) {
         multi(draw_command, N+2, &draws[0].info, 2, 7, sizeof(draws[0]));
      } else if (kind == 2) {
         indexed(draw_command, N+2, &indices[0].info, 2, 7, sizeof(indices[0]), NULL);
      } else {
         int32_t offset = -6;
         indexed(draw_command, N+2, (const void *)compact, 2, 7, sizeof(compact[0]), &offset);
      }
      if (mode >= 4) {
         CHECK(vkEndCommandBuffer(secondary));
         vkCmdExecuteCommands(command, 1, &secondary);
      }
      vkCmdEndRenderPass(command);
      VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
      VkBufferImageCopy copy = { .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
         .imageExtent = {W,H,1} };
      vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_GENERAL, buffers[0], 1, &copy);
      CHECK(vkEndCommandBuffer(command));
      VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1, .pCommandBuffers = &command };
      CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
      CHECK(vkQueueWaitIdle(queue));
      uint8_t *bytes = maps[0];
      for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) {
         unsigned cell = y/8*8+(x%64)/8;
         uint8_t expected[4] = {0,0,0,255};
         if (cell != 37) expected[x < 64 ? 2 : 0] = 255;
         if (memcmp(bytes+4*(y*W+x), expected, 4)) {
            fprintf(stderr, "PIXEL_FAIL mode=%u x=%u y=%u got=%u,%u,%u,%u\n",
               mode,x,y,bytes[4*(y*W+x)],bytes[4*(y*W+x)+1],bytes[4*(y*W+x)+2],bytes[4*(y*W+x)+3]); return 5;
         }
      }
      if (!mode) memcpy(baseline, bytes, sizeof(baseline));
      else if (memcmp(baseline, bytes, sizeof(baseline))) return 5;
      printf("PIXELS mode=%u checked=%u PASS\n", mode, W*H);
   }
   unsigned batch = multi_properties.maxMultiDrawCount < 4096 ? multi_properties.maxMultiDrawCount : 4096;
   VkMultiDrawInfoEXT *many = calloc(batch, sizeof(*many));
   if (!many) return 2;
   for (unsigned i = 0; i < batch; i++) many[i].vertexCount = 6;
   double times[2][24];
   for (unsigned trial = 0; trial < 28; trial++) for (unsigned arm = 0; arm < 2; arm++) {
      unsigned mode = arm ^ (trial & 1);
      CHECK(vkResetCommandBuffer(command, 0));
      CHECK(vkBeginCommandBuffer(command, &begin));
      vkCmdBeginRenderPass(command, &begin_pass, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
      uint64_t start = now();
      if (mode) multi(command, batch, many, 2, 7, sizeof(*many));
      else for (unsigned i = 0; i < batch; i++) vkCmdDraw(command, 6, 2, 0, 7);
      double elapsed = (now()-start)/1e6;
      vkCmdEndRenderPass(command);
      CHECK(vkEndCommandBuffer(command));
      if (trial >= 4) times[mode][trial-4] = elapsed;
   }
   for (unsigned mode = 0; mode < 2; mode++) {
      qsort(times[mode], 24, sizeof(double), compare_double);
      printf("RECORD mode=%s draws=%u median_ms=%.4f p95_ms=%.4f\n",
         mode ? "multi" : "ordinary", batch, (times[mode][11]+times[mode][12])/2, times[mode][22]);
   }
   free(many);
   vkDestroyCommandPool(device, pool, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   for (unsigned i = 0; i < 2; i++) vkDestroyShaderModule(device, modules[i], NULL);
   vkDestroyFramebuffer(device, framebuffer, NULL); vkDestroyRenderPass(device, pass, NULL);
   vkDestroyPipelineLayout(device, layout, NULL);
   vkDestroyDescriptorPool(device, descriptor_pool, NULL);
   vkDestroyDescriptorSetLayout(device, set_layout, NULL);
   for (unsigned i = 0; i < 3; i++) {
      vkUnmapMemory(device, memories[i]); vkDestroyBuffer(device, buffers[i], NULL);
      vkFreeMemory(device, memories[i], NULL);
   }
   vkDestroyImageView(device, view, NULL); vkDestroyImage(device, image, NULL);
   vkFreeMemory(device, image_memory, NULL);
   vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
   return 0;
}
