/* SPDX-License-Identifier: MIT */
#include "rd_private.h"
#include "vk_pipeline.h"
#include "vk_pipeline_cache.h"
#include "vk_format.h"
#include "compiler/nir/nir_serialize.h"
#include "ir3/ir3_compiler.h"
#include "ir3/ir3_nir.h"
#include "util/blob.h"
#include "util/build_id.h"
#include <cstdlib>
#include <cstring>


static constexpr size_t meta_a = offsetof(rd_pipeline, descriptor_count);
static constexpr size_t meta_a_size = offsetof(rd_pipeline, sampling_nir) - meta_a;
static constexpr size_t meta_b = offsetof(rd_pipeline, sampling_mask);
static constexpr size_t meta_b_size = sizeof(rd_pipeline) - meta_b;
static constexpr uint32_t cache_version = 4;
static constexpr size_t cache_max_entry = 64 * 1024 * 1024;
struct rd_cache_object {
   vk_pipeline_cache_object base;
   uint8_t key[BLAKE3_OUT_LEN];
   uint32_t size;
   uint8_t data[];
};
static void digest(const void *data, size_t size, uint8_t *out)
{
   blake3_hasher ctx;
   _mesa_blake3_init(&ctx);
   _mesa_blake3_update(&ctx, data, size);
   _mesa_blake3_final(&ctx, out);
}
static bool rd_cache_key(rd_device *device, rd_pipeline *pipeline,
                      const VkPipelineShaderStageCreateInfo *stage,
                      VkPipelineCreateFlags flags, uint8_t *out)
{
   const build_id_note *id = build_id_find_nhdr_for_addr((const void *)rd_cache_key);
   if (!id || !build_id_length(id))
      return false;
   blake3_hasher ctx;
   _mesa_blake3_init(&ctx);
   const uint32_t abi[] = {cache_version, sizeof(rd_pipeline), sizeof(ir3_shader_variant),
      sizeof(ir3_shader_options), sizeof(struct ir3_const_state), uint32_t(meta_a_size), uint32_t(meta_b_size)};
   _mesa_blake3_update(&ctx, abi, sizeof(abi));
   _mesa_blake3_update(&ctx, build_id_data(id), build_id_length(id));
   _mesa_blake3_update(&ctx, device->vk.physical->properties.deviceUUID, VK_UUID_SIZE);
   _mesa_blake3_update(&ctx, &flags, sizeof(flags));
   uint32_t robust = device->vk.enabled_features.robustBufferAccess;
   _mesa_blake3_update(&ctx, &robust, sizeof(robust));
   uint64_t debug = ir3_shader_debug_hash_key();
   _mesa_blake3_update(&ctx, &debug, sizeof(debug));
   if (stage->stage == VK_SHADER_STAGE_COMPUTE_BIT) {
      uint32_t wavesize = rd_real_wavesize();
      _mesa_blake3_update(&ctx, &wavesize, sizeof(wavesize));
   }
   if (stage->stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
      uint32_t wavesize = rd_fragment_wavesize();
      _mesa_blake3_update(&ctx, &wavesize, sizeof(wavesize));
   }
   uint8_t shader[BLAKE3_OUT_LEN];
   vk_pipeline_hash_shader_stage(flags, stage, nullptr, shader);
   _mesa_blake3_update(&ctx, shader, sizeof(shader));
   const vk_pipeline_layout *layout = pipeline->layout;
   _mesa_blake3_update(&ctx, &layout->set_count, sizeof(layout->set_count));
   _mesa_blake3_update(&ctx, &layout->create_flags, sizeof(layout->create_flags));
   for (uint32_t i = 0; i < layout->set_count; i++) {
      const rd_set_layout *set = container_of(layout->set_layouts[i], const rd_set_layout, vk);
      _mesa_blake3_update(&ctx, &set->binding_count, sizeof(set->binding_count));
      _mesa_blake3_update(&ctx, set->bindings, set->binding_count * sizeof(rd_binding));
      _mesa_blake3_update(&ctx, set->vk.blake3, BLAKE3_OUT_LEN);
      _mesa_blake3_update(&ctx, &layout->dynamic_descriptor_offset[i], sizeof(uint32_t));
      
   }
   _mesa_blake3_update(&ctx, &layout->push_range_count, sizeof(layout->push_range_count));
   _mesa_blake3_update(&ctx, layout->push_ranges, layout->push_range_count * sizeof(VkPushConstantRange));
   _mesa_blake3_final(&ctx, out);
   return true;
}

static bool serialize(vk_pipeline_cache_object *base, blob *out)
{
   rd_cache_object *object = container_of(base, rd_cache_object, base);
   return blob_write_uint32(out, object->size) && blob_write_bytes(out, object->data, object->size);
}
static void destroy(vk_device *, vk_pipeline_cache_object *base)
{
   vk_pipeline_cache_object_finish(base);
   free(base);
}
static vk_pipeline_cache_object *deserialize(vk_pipeline_cache *, const void *, size_t, blob_reader *);
static const vk_pipeline_cache_object_ops cache_ops = {serialize, deserialize, destroy};
static vk_pipeline_cache_object *deserialize(vk_pipeline_cache *cache,
    const void *key, size_t key_size, blob_reader *in)
{
   uint32_t size = blob_read_uint32(in);
   if (key_size != BLAKE3_OUT_LEN || in->overrun || size < BLAKE3_OUT_LEN ||
       size > cache_max_entry || size > size_t(in->end - in->current))
      return nullptr;
   const uint8_t *data = static_cast<const uint8_t *>(blob_read_bytes(in, size));
   uint8_t hash[BLAKE3_OUT_LEN];
   digest(data + BLAKE3_OUT_LEN, size - BLAKE3_OUT_LEN, hash);
   if (memcmp(hash, data, sizeof(hash)))
      return nullptr;
   rd_cache_object *object = static_cast<rd_cache_object *>(calloc(1, sizeof(*object) + size));
   if (!object)
      return nullptr;
   memcpy(object->key, key, key_size);
   memcpy(object->data, data, size);
   object->size = size;
   vk_pipeline_cache_object_init(cache->base.device, &object->base, &cache_ops, object->key, sizeof(object->key));
   return &object->base;
}



static bool resource_metadata_valid(const rd_pipeline *saved,
                                    const vk_pipeline_layout *layout,
                                    VkShaderStageFlagBits stage)
{
   const uint32_t count = saved->resource_count;
   const rd_nir_resource_usage *usage = &saved->resource_usage;
   if (!layout || layout->set_count > RD_MAX_SETS || count > RD_MAX_ACTIVE_RESOURCES ||
       saved->descriptor_count != count || saved->buffer_count > 32 ||
       usage->texture_count > 16 || usage->sampler_count > 16 || usage->image_count > 16 ||
       saved->buffer_count + usage->image_count > 32 ||
       saved->sampling_mask || saved->sampling_buffer ||
       (stage != VK_SHADER_STAGE_COMPUTE_BIT && saved->buffer_count > RD_GFX_MAX_BUFFERS))
      return false;
   const uint64_t descriptors = count == 64 ? UINT64_MAX : (UINT64_C(1) << count) - 1;
   if ((saved->used_descriptors | usage->descriptors) & ~descriptors ||
       usage->textures & ~((1u << usage->texture_count) - 1) ||
       usage->samplers & ~((1u << usage->sampler_count) - 1) ||
       usage->images & ~((1u << usage->image_count) - 1))
      return false;
   const size_t boolean_offsets[] = {
      offsetof(rd_pipeline, linear_subgroups), offsetof(rd_pipeline, packed_1d_barrier),
      offsetof(rd_pipeline, linear_barriers), offsetof(rd_pipeline, batch_local_memory),
   };
   for (size_t offset : boolean_offsets)
      if (reinterpret_cast<const uint8_t *>(saved)[offset] > 1)
         return false;
   uint32_t buffers = 0;
   uint32_t buffer_slots = 0, texture_slots = 0, sampler_slots = 0, image_slots = 0;
   for (uint32_t i = 0; i < count; i++) {
      const rd_nir_resource_binding *resource = &saved->resources[i];
      if (resource->descriptor_set >= layout->set_count ||
          !layout->set_layouts[resource->descriptor_set] || resource->descriptor_slot != i ||
          !resource->plane_count || resource->plane_count > 3)
         return false;
      const rd_set_layout *set = container_of(layout->set_layouts[resource->descriptor_set], rd_set_layout, vk);
      if (resource->logical_slot >= set->descriptor_count || resource->logical_slot >= RD_MAX_BINDINGS)
         return false;
      const rd_binding *binding = nullptr;
      for (uint32_t j = 0; j < set->binding_count; j++)
         if (set->bindings[j].binding == resource->binding)
            binding = &set->bindings[j];
      if (!binding || binding->type != resource->type ||
          resource->array_element >= binding->count ||
          resource->logical_slot != uint64_t(binding->slot) + resource->array_element ||
          !(binding->stages & stage))
         return false;
      uint32_t planes = 1;
      if (binding->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
          binding->immutable_offset != UINT32_MAX) {
         if (binding->immutable_offset >= set->immutable_count ||
             resource->array_element >= set->immutable_count - binding->immutable_offset)
            return false;
         const vk_sampler_state *sampler = &set->immutable_sampler_states[binding->immutable_offset + resource->array_element];
         if (sampler->has_ycbcr_conversion)
            planes = vk_format_get_plane_count(sampler->ycbcr_conversion.format);
      }
      if (resource->plane_count != planes)
         return false;
      bool buffer = false, texture = false, sampler = false, image = false;
      switch (resource->type) {
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: buffer = true; break;
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: texture = sampler = true; break;
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: texture = true; break;
      case VK_DESCRIPTOR_TYPE_SAMPLER: sampler = true; break;
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: image = true; break;
      default: return false;
      }
      if (buffer) {
         uint32_t slot = resource->buffer_slot;
         if (slot >= saved->buffer_count || saved->buffer_descriptors[slot] != i ||
             (buffer_slots & (1u << slot)))
            return false;
         buffer_slots |= 1u << slot;
         buffers++;
      } else if (resource->buffer_slot != UINT32_MAX) {
         return false;
      }
      if (texture) {
         uint32_t slot = resource->texture_slot;
         if (slot >= 16 || planes > 16 - slot)
            return false;
         for (uint32_t p = 0; p < planes; p++) {
            if (saved->texture_descriptors[slot + p] != i || saved->texture_planes[slot + p] != p ||
                (texture_slots & (1u << (slot + p))))
               return false;
            texture_slots |= 1u << (slot + p);
         }
      } else if (resource->texture_slot != UINT32_MAX) {
         return false;
      }
      if (sampler) {
         uint32_t slot = resource->sampler_slot;
         if (slot >= 16 || planes > 16 - slot)
            return false;
         for (uint32_t p = 0; p < planes; p++) {
            if (saved->sampler_descriptors[slot + p] != i || (sampler_slots & (1u << (slot + p))))
               return false;
            sampler_slots |= 1u << (slot + p);
         }
      } else if (resource->sampler_slot != UINT32_MAX) {
         return false;
      }
      if (image) {
         uint32_t slot = resource->image_slot;
         if (slot >= 16 || saved->image_descriptors[slot] != i || (image_slots & (1u << slot)))
            return false;
         image_slots |= 1u << slot;
      } else if (resource->image_slot != UINT32_MAX) {
         return false;
      }
   }
   return buffers == saved->buffer_count && !(usage->textures & ~texture_slots) &&
          !(usage->samplers & ~sampler_slots) && !(usage->images & ~image_slots);
}

static bool variant_resources_valid(const rd_pipeline *saved, const ir3_shader_variant *variant,
                                    const struct ir3_const_state *state)
{
   const ir3_ibo_mapping *mapping = &variant->image_mapping;
   if (variant->inputs_count > ARRAY_SIZE(variant->inputs) ||
       variant->outputs_count > ARRAY_SIZE(variant->outputs) ||
       mapping->num_tex > ARRAY_SIZE(mapping->tex_to_image) ||
       mapping->tex_base > 32 || mapping->num_tex > 32 - mapping->tex_base ||
       variant->num_ssbos > 32 || saved->resource_usage.image_count > 32 - variant->num_ssbos)
      return false;
   for (uint32_t i = 0; i < mapping->num_tex; i++) {
      uint32_t resource = mapping->tex_to_image[i];
      if (resource & UAV_SSBO) {
         if ((resource & ~UAV_SSBO) >= saved->buffer_count)
            return false;
      } else if (resource >= saved->resource_usage.image_count) {
         return false;
      }
   }
   if (!state)
      return true;
   if (state->image_dims.count > 128 || state->image_dims.mask & ~0xffffu ||
       state->ubo_state.num_enabled > ARRAY_SIZE(state->ubo_state.range))
      return false;
   for (uint32_t i = 0; i < 16; i++)
      if ((state->image_dims.mask & (1u << i)) &&
          (i >= saved->resource_usage.image_count || state->image_dims.off[i] > 128 - 6))
         return false;
   for (uint32_t i = 0; i < IR3_MAX_SHADER_BUFFERS; i++)
      if ((state->ssbo_size_mask & (1u << i)) &&
          (i >= saved->buffer_count || state->ssbo_size_offset[i] >= 128))
         return false;
   return true;
}

static bool variant_bounds(rd_device *device, const rd_pipeline *saved, const void *data, size_t size,
                           VkShaderStageFlagBits stage)
{
   blob_reader in;
   blob_reader_init(&in, data, size);
   ir3_shader_key key = {};
   blob_copy_bytes(&in, &key, sizeof(key));
   uint32_t type = blob_read_uint32(&in);
   uint32_t expected = stage == VK_SHADER_STAGE_VERTEX_BIT ? MESA_SHADER_VERTEX :
                       stage == VK_SHADER_STAGE_FRAGMENT_BIT ? MESA_SHADER_FRAGMENT :
                       stage == VK_SHADER_STAGE_COMPUTE_BIT ? MESA_SHADER_COMPUTE : UINT32_MAX;
   if (in.overrun || type != expected || expected == UINT32_MAX ||
       blob_read_uint8(&in) > 1)
      return false;
   if (type != MESA_SHADER_COMPUTE) {
      const ir3_shader_key zero = {};
      if (memcmp(&key, &zero, sizeof(key)))
         return false;
   }
   const unsigned variants = type == MESA_SHADER_VERTEX && ir3_has_binning_vs(&key) ? 2 : 1;
   for (unsigned i = 0; i < variants; i++) {
      ir3_shader_variant v = {};
      blob_copy_bytes(&in, VARIANT_CACHE_PTR(&v), VARIANT_CACHE_SIZE);
      if (in.overrun || !v.info.size || v.info.size > size_t(in.end - in.current))
         return false;
      blob_skip_bytes(&in, v.info.size);
      struct ir3_const_state state = {};
      if (!i)
         blob_copy_bytes(&in, &state, sizeof(state));
      if (in.overrun || !variant_resources_valid(saved, &v, i ? nullptr : &state))
         return false;
      if (!device->compiler->info->props.load_shader_consts_via_preamble) {
         uint32_t count = blob_read_uint32(&in);
         if (in.overrun || count > size_t(in.end - in.current) / sizeof(uint32_t))
            return false;
         blob_skip_bytes(&in, size_t(count) * sizeof(uint32_t));
      }
   }
   return !in.overrun && in.current == in.end;
}

extern "C" bool
rd_pipeline_cache_restore(rd_device *device, rd_pipeline *pipeline,
    VkPipelineCache handle, const VkPipelineShaderStageCreateInfo *stage, VkPipelineCreateFlags flags)
{
   VK_FROM_HANDLE(vk_pipeline_cache, cache, handle);
   uint8_t key[BLAKE3_OUT_LEN];
   if (!cache || !rd_cache_key(device, pipeline, stage, flags, key))
      return false;
   bool hit;
   vk_pipeline_cache_object *base = vk_pipeline_cache_lookup_object(cache, key, sizeof(key), &cache_ops, &hit);
   if (!base)
      return false;
   rd_cache_object *object = container_of(base, rd_cache_object, base);
   blob_reader in;
   blob_reader_init(&in, object->data + BLAKE3_OUT_LEN, object->size - BLAKE3_OUT_LEN);
   rd_pipeline saved = {};
   blob_copy_bytes(&in, reinterpret_cast<uint8_t *>(&saved) + meta_a, meta_a_size);
   blob_copy_bytes(&in, reinterpret_cast<uint8_t *>(&saved) + meta_b, meta_b_size);
   ir3_shader_options options;
   blob_copy_bytes(&in, &options, sizeof(options));
   uint32_t native_size = blob_read_uint32(&in);
   uint32_t nir_size = blob_read_uint32(&in);
   bool valid = !in.overrun && resource_metadata_valid(&saved, pipeline->layout, stage->stage) &&
       saved.compile_flags == flags &&
      uint64_t(native_size) + nir_size == uint64_t(in.end - in.current) &&
       variant_bounds(device, &saved, in.current, native_size, stage->stage);
   if (stage->stage != VK_SHADER_STAGE_COMPUTE_BIT)
      valid = valid && !nir_size && saved.buffer_count <= RD_GFX_MAX_BUFFERS;
   const ir3_shader_variant *variant = nullptr;
   nir_shader *nir = nullptr;
   if (valid) {
      blob_reader native;
      blob_reader_init(&native, in.current, native_size);
      ir3_blob_read reader{&native, device->compiler};
      reader.variant(variant);
      valid = !native.overrun && native.current == native.end && variant;
      if (valid && nir_size) {
         blob_reader nir_blob;
         blob_reader_init(&nir_blob, in.current + native_size, nir_size);
         nir = nir_deserialize(nullptr, ir3_get_compiler_options(device->compiler), &nir_blob);
         valid = nir && !nir_blob.overrun && nir_blob.current == nir_blob.end;
      }
   }
   if (valid) {
      memcpy(reinterpret_cast<uint8_t *>(pipeline) + meta_a, reinterpret_cast<uint8_t *>(&saved) + meta_a, meta_a_size);
      memcpy(reinterpret_cast<uint8_t *>(pipeline) + meta_b, reinterpret_cast<uint8_t *>(&saved) + meta_b, meta_b_size);
      pipeline->variant = const_cast<ir3_shader_variant *>(variant);
      pipeline->variant->shader_options = options;
      pipeline->sampling_nir = nir;
   } else {
      ralloc_free(const_cast<ir3_shader_variant *>(variant));
      ralloc_free(nir);
   }
   vk_pipeline_cache_object_unref(&device->vk, base);
   return valid;
}

extern "C" void
rd_pipeline_cache_store(rd_device *device, rd_pipeline *pipeline,
    VkPipelineCache handle, const VkPipelineShaderStageCreateInfo *stage, VkPipelineCreateFlags flags)
{
   VK_FROM_HANDLE(vk_pipeline_cache, cache, handle);
   uint8_t key[BLAKE3_OUT_LEN];
   if (!cache || !rd_cache_key(device, pipeline, stage, flags, key))
      return;
   blob native, nir, payload;
   blob_init(&native); blob_init(&nir); blob_init(&payload);
   ir3_blob_write writer{&native};
   const ir3_shader_variant *variant = pipeline->variant;
   writer.variant(variant);
   if (pipeline->sampling_nir)
      nir_serialize(&nir, pipeline->sampling_nir, false);
   uint8_t hash[BLAKE3_OUT_LEN] = {};
   blob_write_bytes(&payload, hash, sizeof(hash));
   blob_write_bytes(&payload, reinterpret_cast<const uint8_t *>(pipeline) + meta_a, meta_a_size);
   blob_write_bytes(&payload, reinterpret_cast<const uint8_t *>(pipeline) + meta_b, meta_b_size);
   blob_write_bytes(&payload, &variant->shader_options, sizeof(variant->shader_options));
   blob_write_uint32(&payload, native.size); blob_write_uint32(&payload, nir.size);
   blob_write_bytes(&payload, native.data, native.size); blob_write_bytes(&payload, nir.data, nir.size);
   if (!native.out_of_memory && !nir.out_of_memory && !payload.out_of_memory && payload.size <= cache_max_entry) {
      digest(payload.data + sizeof(hash), payload.size - sizeof(hash), hash);
      memcpy(payload.data, hash, sizeof(hash));
      blob serialized;
      blob_init(&serialized);
      blob_write_uint32(&serialized, payload.size);
      blob_write_bytes(&serialized, payload.data, payload.size);
      if (!serialized.out_of_memory) {
         vk_pipeline_cache_object *object = vk_pipeline_cache_create_and_insert_object(cache,
            key, sizeof(key), serialized.data, serialized.size, &cache_ops);
         if (object)
            vk_pipeline_cache_object_unref(&device->vk, object);
      }
      blob_finish(&serialized);
   }
   blob_finish(&payload); blob_finish(&nir); blob_finish(&native);
}
