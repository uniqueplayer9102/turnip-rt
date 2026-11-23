#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#ifndef W
#define W 1024u
#endif
#ifndef H
#define H 2048u
#endif
#ifndef SOURCE_FORMAT
#define SOURCE_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT
#endif
#define CHECK(call) do { VkResult r=(call); if (r!=VK_SUCCESS) { fprintf(stderr,"%s: %d\n",#call,r); exit(1); } } while (0)
static VkDevice device;
static VkDeviceMemory memory(VkMemoryRequirements req)
{
   VkMemoryAllocateInfo info={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=0};
   VkDeviceMemory out;
   CHECK(vkAllocateMemory(device,&info,NULL,&out));
   return out;
}
static VkShaderModule shader(const char *path)
{
   FILE *file=fopen(path,"rb");
   if (!file) exit(2);
   fseek(file,0,SEEK_END);
   long size=ftell(file);
   rewind(file);
   if (size<=0 || size%4) exit(2);
   uint32_t *code=malloc(size);
   if (!code || fread(code,1,size,file)!=(size_t)size) exit(2);
   fclose(file);
   VkShaderModuleCreateInfo info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=size,.pCode=code};
   VkShaderModule out;
   CHECK(vkCreateShaderModule(device,&info,NULL,&out));
   free(code);
   return out;
}
int main(int argc,char **argv)
{
   if (argc!=3 && argc!=4 && argc!=5) return 2;
   int gpu=argc==5;
   int mixed=gpu && getenv("RADISH_PROBE_MIXED");
   int empty=mixed && getenv("RADISH_PROBE_EMPTY");
   VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
   VkInstanceCreateInfo instance_info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
   VkInstance instance;
   CHECK(vkCreateInstance(&instance_info,NULL,&instance));
   VkPhysicalDevice physical;
   uint32_t count=1;
   CHECK(vkEnumeratePhysicalDevices(instance,&count,&physical));
   float priority=1;
   VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=0,.queueCount=1,.pQueuePriorities=&priority};
   VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
   CHECK(vkCreateDevice(physical,&di,NULL,&device));
   VkQueue queue;
   vkGetDeviceQueue(device,0,0,&queue);
   VkImage images[2]; VkImageView views[2]; VkDeviceMemory image_memory[2];
   unsigned source_cpp=SOURCE_FORMAT==VK_FORMAT_R16G16B16A16_SFLOAT?8:4;
   VkFormat formats[]={SOURCE_FORMAT,VK_FORMAT_B8G8R8A8_UNORM};
   for (unsigned i=0;i<2;i++) {
      VkImageCreateInfo info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=formats[i],
         .extent={W,H,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
         .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|(i ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT|(gpu?0:VK_IMAGE_USAGE_TRANSFER_DST_BIT)),
         .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
      CHECK(vkCreateImage(device,&info,NULL,&images[i]));
      VkMemoryRequirements req;
      vkGetImageMemoryRequirements(device,images[i],&req);
      image_memory[i]=memory(req);
      CHECK(vkBindImageMemory(device,images[i],image_memory[i],0));
      VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=images[i],.viewType=VK_IMAGE_VIEW_TYPE_2D,
         .format=formats[i],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
      CHECK(vkCreateImageView(device,&vi,NULL,&views[i]));
   }
   VkBuffer buffers[2]; VkDeviceMemory buffer_memory[2]; void *maps[2];
   for (unsigned i=0;i<2;i++) {
      VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=(VkDeviceSize)W*H*(i?4:source_cpp),
         .usage=i?VK_BUFFER_USAGE_TRANSFER_DST_BIT:VK_BUFFER_USAGE_TRANSFER_SRC_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
      CHECK(vkCreateBuffer(device,&bi,NULL,&buffers[i]));
      VkMemoryRequirements req;
      vkGetBufferMemoryRequirements(device,buffers[i],&req);
      buffer_memory[i]=memory(req);
      CHECK(vkBindBufferMemory(device,buffers[i],buffer_memory[i],0));
      CHECK(vkMapMemory(device,buffer_memory[i],0,VK_WHOLE_SIZE,0,&maps[i]));
   }
   VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
   VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
   VkDescriptorSetLayout layout;
   CHECK(vkCreateDescriptorSetLayout(device,&li,NULL,&layout));
   VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,1};
   VkDescriptorPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
   VkDescriptorPool pool;
   CHECK(vkCreateDescriptorPool(device,&pi,NULL,&pool));
   VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&layout};
   VkDescriptorSet set;
   CHECK(vkAllocateDescriptorSets(device,&ai,&set));
   VkDescriptorImageInfo image_info={.imageView=views[0],.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
   VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,.descriptorCount=1,
      .descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,.pImageInfo=&image_info};
   vkUpdateDescriptorSets(device,1,&write,0,NULL);
   VkPipelineLayoutCreateInfo pli={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&layout};
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(device,&pli,NULL,&pl));
   VkAttachmentDescription attachments[2]={0};
   for (unsigned i=0;i<2;i++) {
      attachments[i]=(VkAttachmentDescription){.format=formats[i],.samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=i?VK_ATTACHMENT_LOAD_OP_DONT_CARE:VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout=VK_IMAGE_LAYOUT_GENERAL,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
   }
   VkAttachmentReference source={0,VK_IMAGE_LAYOUT_GENERAL},target={1,VK_IMAGE_LAYOUT_GENERAL};
   VkSubpassDescription subpasses[2]={
      {.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&source},
      {.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&target,.inputAttachmentCount=1,.pInputAttachments=&source}};
   VkSubpassDependency dependency={.srcSubpass=0,.dstSubpass=1,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
      .dependencyFlags=VK_DEPENDENCY_BY_REGION_BIT};
   VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=2,.pAttachments=attachments,
      .subpassCount=2,.pSubpasses=subpasses,.dependencyCount=1,.pDependencies=&dependency};
   VkRenderPass pass;
   CHECK(vkCreateRenderPass(device,&ri,NULL,&pass));
   VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,.attachmentCount=2,.pAttachments=views,.width=W,.height=H,.layers=1};
   VkFramebuffer framebuffer;
   CHECK(vkCreateFramebuffer(device,&fi,NULL,&framebuffer));
   VkShaderModule modules[]={shader(argv[1]),shader(argv[2])};
   VkPipelineShaderStageCreateInfo stages[2]={
      {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=modules[0],.pName="main"},
      {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=modules[1],.pName="main"}};
   VkPipelineVertexInputStateCreateInfo vertex={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport viewport={0,0,W,H,0,1}; VkRect2D scissor={{0,0},{W,H}};
   VkPipelineViewportStateCreateInfo vpi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
   VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.lineWidth=1};
   VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState blend={.colorWriteMask=15};
   VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&blend};
   VkGraphicsPipelineCreateInfo gpi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
      .pVertexInputState=&vertex,.pInputAssemblyState=&assembly,.pViewportState=&vpi,.pRasterizationState=&raster,.pMultisampleState=&ms,
      .pColorBlendState=&cb,.layout=pl,.renderPass=pass,.subpass=1};
   VkPipeline pipeline;
   CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gpi,NULL,&pipeline));
   VkShaderModule pattern_module=VK_NULL_HANDLE;
   VkPipeline pattern_pipeline=VK_NULL_HANDLE;
   VkPipeline partial_pipeline=VK_NULL_HANDLE;
   if (gpu) {
      pattern_module=shader(argv[4]);
      stages[1].module=pattern_module;
      gpi.subpass=0;
      CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gpi,NULL,&pattern_pipeline));
      if (mixed) {
         uint32_t phase=1;
         VkSpecializationMapEntry entry={.constantID=0,.offset=0,.size=sizeof(phase)};
         VkSpecializationInfo specialization={.mapEntryCount=1,.pMapEntries=&entry,
            .dataSize=sizeof(phase),.pData=&phase};
         stages[1].pSpecializationInfo=&specialization;
         const VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
         VkPipelineDynamicStateCreateInfo dy={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount=2,.pDynamicStates=dynamic};
         gpi.pDynamicState=&dy;
         CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gpi,NULL,&partial_pipeline));
      }
   }
   VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=0};
   VkCommandPool commands;
   CHECK(vkCreateCommandPool(device,&cpi,NULL,&commands));
   VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
   VkCommandBuffer command;
   CHECK(vkAllocateCommandBuffers(device,&cai,&command));
   unsigned frames=getenv("RADISH_PROBE_FRAMES")?strtoul(getenv("RADISH_PROBE_FRAMES"),NULL,10):2;
   if (frames<2 || frames>128) return 2;
   for (unsigned frame=0;frame<frames;frame++) {
      if (!gpu) {
         uint16_t *upload=maps[0];
         for (unsigned y=0;y<H;y++) for (unsigned x=0;x<W;x++) {
            unsigned at=4*(y*W+x);
            if (source_cpp==8) {
               upload[at]=(((x/17)+frame)&1)?0x3c00:0;
               upload[at+1]=(((y/29)+frame)&1)?0x3c00:0;
               upload[at+2]=((((x+y)/31)+frame)&1)?0x3c00:0;
               upload[at+3]=0x3c00;
            } else {
               uint8_t *bytes=maps[0];
               bytes[at]=(((x/17)+frame)&1)?255:0;
               bytes[at+1]=(((y/29)+frame)&1)?255:0;
               bytes[at+2]=((((x+y)/31)+frame)&1)?255:0;
               bytes[at+3]=255;
            }
         }
      }
      CHECK(vkResetCommandBuffer(command,0));
      VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      CHECK(vkBeginCommandBuffer(command,&begin));
      if (!frame) {
         VkImageMemoryBarrier barriers[2]={0};
         for (unsigned i=0;i<2;i++) barriers[i]=(VkImageMemoryBarrier){.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.image=images[i],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
         vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,2,barriers);
      }
      VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={W,H,1}};
      if (!gpu)
         vkCmdCopyBufferToImage(command,buffers[0],images[0],VK_IMAGE_LAYOUT_GENERAL,1,&copy);
      VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT};
      vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,1,&barrier,0,NULL,0,NULL);
      VkRenderPassBeginInfo bi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=framebuffer,.renderArea={{0,0},{W,H}}};
      if (gpu && frame)
         bi.renderArea=(VkRect2D){{13,19},{W-40,H-60}};
      vkCmdBeginRenderPass(command,&bi,VK_SUBPASS_CONTENTS_INLINE);
      if (gpu && !frame) {
         vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pattern_pipeline);
         vkCmdDraw(command,3,1,0,0);
      }
      if (mixed && frame) {
         VkRect2D partial={{800,1000},{128,256}};
         if (empty && frame>1)
            partial.extent=(VkExtent2D){0,0};
         vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,partial_pipeline);
         vkCmdSetViewport(command,0,1,&viewport);
         vkCmdSetScissor(command,0,1,&partial);
         vkCmdDraw(command,3,1,0,0);
      }
      if (argc==4 || (gpu && !(empty && frame>1))) {
         VkClearAttachment clear={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.colorAttachment=0};
         clear.clearValue.color.float32[frame?2:0]=1;
         clear.clearValue.color.float32[3]=1;
         VkClearRect rect={.rect={{64,96},{704,800}},.baseArrayLayer=0,.layerCount=1};
         vkCmdClearAttachments(command,1,&clear,1,&rect);
      }
      vkCmdNextSubpass(command,VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
      vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pl,0,1,&set,0,NULL);
      vkCmdDraw(command,3,1,0,0);
      vkCmdEndRenderPass(command);
      barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
      vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,NULL,0,NULL);
      vkCmdCopyImageToBuffer(command,images[1],VK_IMAGE_LAYOUT_GENERAL,buffers[1],1,&copy);
      CHECK(vkEndCommandBuffer(command));
      VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
      CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));
      CHECK(vkQueueWaitIdle(queue));
      uint8_t *bytes=maps[1]; uint64_t hash=UINT64_C(1469598103934665603);
      for (unsigned y=0;y<H;y++) for (unsigned x=0;x<W;x++) {
         unsigned pattern_frame=gpu?0:frame;
         uint8_t expected[4]={((((x+y)/31)+pattern_frame)&1)?255:0,(((y/29)+pattern_frame)&1)?255:0,(((x/17)+pattern_frame)&1)?255:0,255};
         if ((argc==4 || gpu) && x>=64 && x<768 && y>=96 && y<896) {
            expected[0]=frame?255:0;expected[1]=0;expected[2]=frame?0:255;expected[3]=255;
         }
         if (mixed && frame && x>=800 && x<928 && y>=1000 && y<1256) {
            expected[0]=255-expected[0];expected[1]=255-expected[1];expected[2]=255-expected[2];
         }
         uint8_t *pixel=bytes+4*(y*W+x);
         if (memcmp(pixel,expected,4)) { fprintf(stderr,"CONVERSION_FAIL frame=%u x=%u y=%u\n",frame,x,y); return 5; }
         for (unsigned i=0;i<4;i++) hash=(hash^pixel[i])*UINT64_C(1099511628211);
      }
      printf("CONVERSION_READBACK frame=%u pixels=%u hash=%016lx PASS\n",frame,W*H,(unsigned long)hash);
   }
   vkDestroyCommandPool(device,commands,NULL);
   vkDestroyPipeline(device,pipeline,NULL);
   if (gpu) {
      if (partial_pipeline)
         vkDestroyPipeline(device,partial_pipeline,NULL);
      vkDestroyPipeline(device,pattern_pipeline,NULL);
      vkDestroyShaderModule(device,pattern_module,NULL);
   }
   for (unsigned i=0;i<2;i++) vkDestroyShaderModule(device,modules[i],NULL);
   vkDestroyFramebuffer(device,framebuffer,NULL); vkDestroyRenderPass(device,pass,NULL);
   vkDestroyPipelineLayout(device,pl,NULL); vkDestroyDescriptorPool(device,pool,NULL); vkDestroyDescriptorSetLayout(device,layout,NULL);
   for (unsigned i=0;i<2;i++) {
      vkUnmapMemory(device,buffer_memory[i]); vkDestroyBuffer(device,buffers[i],NULL); vkFreeMemory(device,buffer_memory[i],NULL);
      vkDestroyImageView(device,views[i],NULL); vkDestroyImage(device,images[i],NULL); vkFreeMemory(device,image_memory[i],NULL);
   }
   vkDestroyDevice(device,NULL); vkDestroyInstance(instance,NULL);
   return 0;
}
