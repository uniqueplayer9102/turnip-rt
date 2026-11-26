#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vulkan/vulkan_core.h>

#define RD_MAX_SETS 4
#define RD_MAX_BINDINGS 1024
#define RD_MAX_ACTIVE_RESOURCES 64
#define IR3_MAX_SHADER_BUFFERS 32
#define UAV_SSBO 0x80
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define container_of(p, type, member) \
   reinterpret_cast<const type *>(reinterpret_cast<const char *>(p) - offsetof(type, member))
struct vk_descriptor_set_layout { uint32_t unused; };
struct vk_pipeline_layout { uint32_t set_count; const vk_descriptor_set_layout *set_layouts[4]; };
struct vk_sampler_state { bool has_ycbcr_conversion; struct { VkFormat format; } ycbcr_conversion; };
struct rd_binding { uint32_t binding, count, slot; VkDescriptorType type; VkShaderStageFlags stages; uint32_t immutable_offset; };
struct rd_set_layout {
   vk_descriptor_set_layout vk;
   uint32_t binding_count, descriptor_count, pool_descriptor_count, immutable_count;
   vk_sampler_state *immutable_sampler_states;
   rd_binding bindings[64];
};
struct rd_nir_resource_binding {
   uint32_t descriptor_set, binding, array_element, logical_slot, descriptor_slot;
   VkDescriptorType type;
   uint32_t buffer_slot, texture_slot, sampler_slot, image_slot, plane_count;
};
struct rd_nir_resource_usage {
   uint64_t descriptors;
   uint32_t textures, samplers, images, texture_count, sampler_count, image_count;
};
struct rd_pipeline {
   uint32_t descriptor_count;
   uint64_t used_descriptors;
   bool linear_subgroups, packed_1d_barrier, linear_barriers, batch_local_memory;
   uint32_t sampling_mask, sampling_buffer;
   rd_nir_resource_binding resources[64];
   uint32_t resource_count;
   rd_nir_resource_usage resource_usage;
   uint32_t texture_descriptors[16], sampler_descriptors[16], image_descriptors[16];
   uint8_t texture_planes[16];
   uint32_t buffer_count, buffer_descriptors[32];
};
struct ir3_ibo_mapping { uint32_t num_tex, tex_base; uint8_t tex_to_image[32]; };
struct ir3_shader_variant {
   uint32_t inputs_count, outputs_count, inputs[64], outputs[64], num_ssbos;
   ir3_ibo_mapping image_mapping;
};
struct ir3_const_state {
   struct { uint32_t count, mask, off[16]; } image_dims;
   struct { uint32_t num_enabled, range[8]; } ubo_state;
   uint32_t ssbo_size_mask, ssbo_size_offset[32];
};
static uint32_t vk_format_get_plane_count(VkFormat format)
{
   return format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM ? 2 : 1;
}
#define RD_GFX_MAX_BUFFERS 20
#include "cache_under_test.h"

static void setup(rd_pipeline &pipeline, rd_set_layout &set, VkDescriptorType type)
{
   pipeline = {}; set = {};
   set.binding_count = set.descriptor_count = 1;
   set.bindings[0] = {0, 1, 0, type, VK_SHADER_STAGE_ALL, UINT32_MAX};
   pipeline.resource_count = pipeline.descriptor_count = 1;
   pipeline.used_descriptors = pipeline.resource_usage.descriptors = 1;
   pipeline.resources[0] = {0, 0, 0, 0, 0, type, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, 1};
   auto &resource = pipeline.resources[0];
   if (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
      pipeline.buffer_count = 1; resource.buffer_slot = 0;
   } else if (type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
      pipeline.resource_usage.texture_count = pipeline.resource_usage.sampler_count = 1;
      pipeline.resource_usage.textures = pipeline.resource_usage.samplers = 1;
      resource.texture_slot = resource.sampler_slot = 0;
   } else if (type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
      pipeline.resource_usage.image_count = pipeline.resource_usage.images = 1;
      resource.image_slot = 0;
   }
}

int main()
{
   rd_pipeline good = {}; rd_set_layout set = {};
   vk_pipeline_layout layout = {1, {&set.vk}};
   setup(good, set, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
   auto valid = [&](const rd_pipeline &p) {
      return resource_metadata_valid(&p, &layout, VK_SHADER_STAGE_COMPUTE_BIT);
   };
   assert(valid(good));
   rd_pipeline bad = good;
   bad.resources[0].descriptor_set = 4; assert(!valid(bad));
   bad = good; bad.resources[0].logical_slot = 1024; assert(!valid(bad));
   bad = good; bad.resources[0].descriptor_slot = 1; assert(!valid(bad));
   bad = good; bad.resources[0].array_element = 1; assert(!valid(bad));
   bad = good; bad.resources[0].binding = 9; assert(!valid(bad));
   bad = good; bad.resources[0].buffer_slot = 32; assert(!valid(bad));
   bad = good; bad.buffer_descriptors[0] = UINT32_MAX; assert(!valid(bad));
   bad = good; bad.sampling_mask = 1; assert(!valid(bad));
   bad = good; bad.sampling_buffer = UINT32_MAX; assert(!valid(bad));
   bad = good; bad.used_descriptors = 2; assert(!valid(bad));
   bad = good; bad.resource_usage.descriptors = 2; assert(!valid(bad));
   bad = good; bad.resources[0].plane_count = 0; assert(!valid(bad));
   bad = good; bad.resources[0].texture_slot = 0; assert(!valid(bad));
   bad = good;
   reinterpret_cast<uint8_t *>(&bad)[offsetof(rd_pipeline, linear_subgroups)] = 2;
   assert(!valid(bad));
   set.bindings[0].stages = VK_SHADER_STAGE_VERTEX_BIT; assert(!valid(good));
   set.bindings[0].stages = VK_SHADER_STAGE_ALL;

   setup(good, set, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
   assert(valid(good));
   bad = good; bad.texture_descriptors[0] = 63; assert(!valid(bad));
   bad = good; bad.sampler_descriptors[0] = 63; assert(!valid(bad));
   bad = good; bad.texture_planes[0] = 1; assert(!valid(bad));
   bad = good; bad.resource_usage.textures = 2; assert(!valid(bad));
   vk_sampler_state sampler = {true, {VK_FORMAT_G8_B8R8_2PLANE_420_UNORM}};
   set.immutable_sampler_states = &sampler; set.immutable_count = 1;
   set.bindings[0].immutable_offset = 0;
   good.resources[0].plane_count = 2;
   good.resource_usage.texture_count = good.resource_usage.sampler_count = 2;
   good.resource_usage.textures = good.resource_usage.samplers = 3;
   good.texture_planes[1] = 1;
   assert(valid(good));
   bad = good; bad.texture_planes[1] = 0; assert(!valid(bad));
   set.bindings[0].immutable_offset = 1; assert(!valid(good));

   setup(good, set, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
   assert(valid(good));
   bad = good; bad.image_descriptors[0] = UINT32_MAX; assert(!valid(bad));
   bad = good; bad.resources[0].image_slot = 16; assert(!valid(bad));

   setup(good, set, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
   ir3_shader_variant variant = {}; variant.num_ssbos = 1;
   ir3_const_state state = {};
   assert(variant_resources_valid(&good, &variant, &state));
   variant.image_mapping.num_tex = 1;
   variant.image_mapping.tex_to_image[0] = UAV_SSBO;
   assert(variant_resources_valid(&good, &variant, &state));
   variant.image_mapping.tex_to_image[0] = UAV_SSBO | 1;
   assert(!variant_resources_valid(&good, &variant, &state));
   variant.image_mapping.tex_to_image[0] = 0;
   assert(!variant_resources_valid(&good, &variant, &state));
   variant.image_mapping.num_tex = 0;
   variant.inputs_count = 65; assert(!variant_resources_valid(&good, &variant, &state));
   variant.inputs_count = 0;
   variant.outputs_count = 65; assert(!variant_resources_valid(&good, &variant, &state));
   variant.outputs_count = 0;
   state.ubo_state.num_enabled = 9; assert(!variant_resources_valid(&good, &variant, &state));
   state = {}; state.image_dims.count = 129;
   assert(!variant_resources_valid(&good, &variant, &state));
   state = {}; state.ssbo_size_mask = 1; state.ssbo_size_offset[0] = 128;
   assert(!variant_resources_valid(&good, &variant, &state));

   good = {}; set = {};
   set.binding_count = set.descriptor_count = 64;
   good.resource_count = good.descriptor_count = 64;
   good.used_descriptors = good.resource_usage.descriptors = UINT64_MAX;
   good.buffer_count = 32;
   good.resource_usage.texture_count = good.resource_usage.sampler_count = 16;
   good.resource_usage.textures = good.resource_usage.samplers = 0xffff;
   for (uint32_t i = 0; i < 64; i++) {
      VkDescriptorType type = i < 32 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER :
                             i < 48 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
      set.bindings[i] = {i, 1, i, type, VK_SHADER_STAGE_ALL, UINT32_MAX};
      auto &r = good.resources[i];
      r = {0, i, 0, i, i, type, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, 1};
      if (i < 32) { r.buffer_slot = i; good.buffer_descriptors[i] = i; }
      else if (i < 48) { r.texture_slot = i - 32; good.texture_descriptors[i - 32] = i; }
      else { r.sampler_slot = i - 48; good.sampler_descriptors[i - 48] = i; }
   }
   assert(valid(good));
   assert(!resource_metadata_valid(&good, &layout, VK_SHADER_STAGE_FRAGMENT_BIT));
   bad = good; bad.resource_count = bad.descriptor_count = 63; assert(!valid(bad));
   std::puts("CACHE_METADATA PASS: descriptor indices, layouts, planes, masks, sampling metadata, native mappings and 64-resource bounds");
}
