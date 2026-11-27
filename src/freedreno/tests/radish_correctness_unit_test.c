#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <vulkan/vulkan_core.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#define MAX2(a, b) ((a) > (b) ? (a) : (b))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define align64(v, a) (((v) + (a) - 1) & ~((uint64_t)(a) - 1))
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
struct list_head { struct list_head *next, *prev; };
static void list_inithead(struct list_head *h) { h->next = h->prev = h; }
static bool list_is_empty(const struct list_head *h) { return h->next == h; }
static void list_addtail(struct list_head *n, struct list_head *h)
{
   n->prev = h->prev; n->next = h; h->prev->next = n; h->prev = n;
}
static void list_delinit(struct list_head *n)
{
   n->prev->next = n->next; n->next->prev = n->prev; list_inithead(n);
}
#define list_for_each_entry(type, v, h, member) \
   for (type *v = container_of((h)->next, type, member); &v->member != (h); \
        v = container_of(v->member.next, type, member))
struct util_dynarray { void *data; unsigned size, capacity; };
static bool fail_growth;
static void *grow_bytes(struct util_dynarray *array, unsigned bytes)
{
   if (fail_growth) return NULL;
   unsigned size = array->size + bytes;
   if (size > array->capacity) {
      void *data = realloc(array->data, size);
      if (!data) return NULL;
      array->data = data; array->capacity = size;
   }
   void *out = (char *)array->data + array->size;
   array->size = size;
   return out;
}
static void util_dynarray_clear(struct util_dynarray *array) { array->size = 0; }
#define util_dynarray_grow(a, type, n) grow_bytes((a), sizeof(type) * (n))
#define util_dynarray_num_elements(a, type) ((size_t)(a)->size / sizeof(type))
#define util_dynarray_foreach(a, type, v) \
   for (type *v = (type *)(a)->data; v < (type *)((char *)(a)->data + (a)->size); v++)

#include "trace_defs_under_test.h"
struct fd_bo { uint64_t *map; uint64_t handle, size; };
struct fd_ringbuffer { unsigned unused; uint32_t *start, *cur, *end; uint32_t size; };
struct fd_submit { unsigned unused; };
struct rd_submit_window { unsigned unused; };
struct rd_query_pool;
struct rd_query_slot {
   struct list_head active_link;
   struct rd_query_pool *pool;
   uint64_t values[11];
   bool available, active;
};
struct rd_query_pool {
   struct { VkQueryType query_type; uint32_t query_count; } vk;
   mtx_t mutex;
   cnd_t condition;
   struct rd_query_slot slots[8];
};
struct rd_render_query {
   struct rd_query_pool *pool;
   uint32_t query;
   bool end;
   uint32_t draw_index;
   bool zero;
};
struct rd_draw {
   uint32_t vertex_count, instance_count;
   struct rd_query_pool *occlusion_pool;
   uint32_t occlusion_query;
};
struct rd_render_attachment { void *resolve, *view; };
struct rd_render {
   struct util_dynarray draws, queries;
   uint32_t view_mask, layers, color_count;
   struct rd_render_attachment color[8], depth, stencil;
   bool descriptor_arena;
};
struct rd_device {
   struct rd_trace *trace;
   uint64_t submit_count;
   uint64_t descriptor_set_serial;
   struct list_head active_queries;
   void *pipe, *fd;
   struct { uint32_t threadsize_base; } info;
   struct fd_bo *border_colors;
   uint64_t border_colors_offset;
};
static uint64_t next_start, next_end;
static int prep_error;
static unsigned prep_calls, deleted_bos;
static struct fd_bo border_bo;
#define FD_BO_PREP_READ 1
#define FD_BO_GPUREADONLY 1
#define OUT_WFI5(ring) ((void)(ring))
static int fd_bo_cpu_prep(struct fd_bo *bo, void *pipe, unsigned flags)
{
   (void)bo; (void)pipe; (void)flags; prep_calls++; return prep_error;
}
static struct fd_bo *fd_bo_new(void *fd, unsigned size, unsigned flags, const char *name)
{
   (void)fd; (void)size; (void)flags; (void)name; return &border_bo;
}
static void *fd_bo_map(struct fd_bo *bo) { (void)bo; return NULL; }
static void fd_bo_del(struct fd_bo *bo) { (void)bo; deleted_bos++; }
static uint64_t fd_bo_get_iova(struct fd_bo *bo) { (void)bo; return 8192; }
static unsigned fd_bo_size(struct fd_bo *bo) { (void)bo; return 16384; }
static uint64_t fd_ticks_to_ns(uint64_t ticks) { return ticks; }
static int64_t os_time_get_nano(void) { static int64_t n; return ++n; }
static bool debug_get_bool_option(const char *name, bool fallback)
{
   (void)name; return fallback;
}
static void emit_timestamp(struct fd_ringbuffer *ring, struct fd_bo *bo, uint32_t offset)
{
   (void)ring;
   bo->map[offset / 8] = offset % 16 ? next_end : next_start;
}
static VkResult rd_draw_counts(struct rd_device *device, const struct rd_draw *draw,
                               uint64_t *vertices, uint64_t *primitives)
{
   (void)device;
   *vertices = (uint64_t)draw->vertex_count * draw->instance_count;
   *primitives = *vertices / 3;
   return VK_SUCCESS;
}

#include "graphics_under_test.h"
#include "trace_under_test.h"
#include "border_under_test.h"

enum a5xx_tex_type { A5XX_TEX_1D, A5XX_TEX_2D, A5XX_TEX_CUBE, A5XX_TEX_3D };
struct rd_resource {
   struct fd_bo *bo;
   VkDeviceSize offset;
   enum a5xx_tex_type type;
   uint32_t cpp, width, height, depth, row_pitch, array_pitch, format_word;
   uint32_t mip_levels, pitchalign, min_layer_size;
   bool buffer, image_size_only;
};
#include "texture_pitch_under_test.h"

typedef unsigned nir_intrinsic_op;
enum gl_access_qualifier { RD_ACCESS_NONE = 0 };
enum nir_mem_access_shift_method { nir_mem_access_shift_method_scalar };
typedef struct nir_mem_access_size_align {
   uint8_t num_components, bit_size;
   uint16_t align;
   enum nir_mem_access_shift_method shift;
} nir_mem_access_size_align;
#include "subword_under_test.h"
#include "blit_src_under_test.h"

struct vk_device_extension_table { bool KHR_swapchain, KHR_incremental_present; };
#include "device_extensions_under_test.h"

struct rd_image_level { VkDeviceSize array_pitch; };
struct vk_image {
   uint32_t array_layers, mip_levels;
   VkImageUsageFlags2KHR usage, stencil_usage;
   VkImageAspectFlags aspects;
   VkFormat format;
   VkImageType image_type;
   VkSampleCountFlagBits samples;
   VkExtent3D extent;
};
struct rd_image {
   struct vk_image vk;
   struct { uint32_t tile_mode; } native_layout;
   struct rd_image_level levels[1];
   struct rd_memory *memory;
   VkDeviceSize memory_offset, size;
};
#include "image_pitch_under_test.h"

/* Registry extension numbers normally supplied by Mesa's generated headers. */
#define _VK_KHR_sampler_ycbcr_conversion_number VK_ENUM_EXTENSION(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
#define _VK_EXT_ycbcr_2plane_444_formats_number VK_ENUM_EXTENSION(VK_FORMAT_G8_B8R8_2PLANE_444_UNORM)
static unsigned u_minify(unsigned value, unsigned levels)
{
   value >>= levels;
   return value ? value : 1;
}
#include "ycbcr_formats_under_test.h"

enum vk_sync_flags { VK_SYNC_IS_TIMELINE = 1 };
struct vk_sync { enum vk_sync_flags flags; };
struct vk_sync_wait { struct vk_sync *sync; };
#include "sync_waits_under_test.h"
#include "sync_policy_under_test.h"

#define RD_MAX_VERTEX_ATTRIBUTES 16
#define RD_MAX_VERTEX_BINDINGS 16
#define RD_MAX_VERTEX_BINDING_STRIDE 2048
#define RD_MAX_VERTEX_ATTRIBUTE_OFFSET 2047
struct VkPipelineVertexInputDivisorStateCreateInfoEXT;
#define vk_find_struct_const(pnext, type) ((void)(pnext), (void *)NULL)
#include "vertex_input_under_test.h"
#include "query_range_under_test.h"

#include "ycbcr_conversion_state_under_test.h"
struct rd_binding { uint32_t slot, count, binding, immutable_offset; VkDescriptorType type; };
struct vk_sampler_state {
   bool has_ycbcr_conversion;
   struct vk_ycbcr_conversion_state ycbcr_conversion;
   uint32_t words[4];
};
struct vk_descriptor_set_layout { uint32_t unused; };
struct rd_set_layout {
   struct vk_descriptor_set_layout vk;
   uint32_t descriptor_count, binding_count, immutable_count;
   struct vk_sampler_state *immutable_sampler_states;
   struct rd_binding bindings[8];
};
struct rd_memory { struct { VkDeviceSize size; } vk; struct fd_bo *bo; };
struct vk_buffer { VkDeviceSize size; };
struct rd_buffer { struct vk_buffer vk; struct rd_memory *memory; VkDeviceSize offset; };

#define RD_MAX_SETS 4
#define RD_MAX_BINDINGS 1024
#define RD_MAX_ACTIVE_RESOURCES 64
#define RD_MAX_PUSH_CONSTANTS_SIZE 256
#define FD_VERSION_SOFTPIN 4
struct ir3_shader_variant {
   uint32_t local_size[3], shared_size, num_ssbos, pvtmem_size, constant_data_size, halfregs;
   bool has_barrier;
   struct { bool double_threadsize; uint32_t preamble_instrs_count; } info;
   struct {
      uint32_t workgroup_resident_threads;
      struct { bool a5xx_ssbo_size_consts; } nir_options;
   } shader_options;
   struct { uint32_t num_tex; } image_mapping;
};
static uint32_t ir3_shader_halfregs(const struct ir3_shader_variant *v) { return v->halfregs; }
static unsigned fd_pipe_dev_id(const void *pipe) { return *(const unsigned *)pipe; }
static unsigned fd_dev_gpu_id(unsigned id) { return id; }
static unsigned fd_device_version(const void *fd) { return *(const unsigned *)fd; }

struct rd_image_view { bool ycbcr; struct vk_ycbcr_conversion_state conversion; };
struct rd_sampler { struct vk_sampler_state state; };
struct rd_descriptor {
   VkDescriptorType type;
   struct rd_buffer *buffer;
   VkDeviceSize offset, range;
   struct rd_image_view *image_view;
   struct rd_buffer_view *buffer_view;
   bool has_sampler;
   uint32_t sampler[4];
};
#include "descriptor_set_version_under_test.h"
struct rd_descriptor_set {
   struct rd_set_layout *layout;
   struct rd_descriptor_set_version version;
   struct rd_descriptor descriptors[2];
};
struct vk_descriptor_update_template {
   uint32_t entry_count;
   struct vk_descriptor_template_entry {
      VkDescriptorType type;
      uint32_t binding, array_element, array_count;
      size_t offset, stride;
   } entries[1];
};
#define RD_TEST_HANDLE(type, obj) ((type)(uintptr_t)(obj))
#define RD_TEST_FROM_HANDLE(type, handle) ((struct type *)(uintptr_t)(handle))
#define VK_FROM_HANDLE(type, name, handle) struct type *name = RD_TEST_FROM_HANDLE(type, handle)
#define rd_buffer_from_handle(h) RD_TEST_FROM_HANDLE(rd_buffer, h)
#define rd_buffer_view_from_handle(h) RD_TEST_FROM_HANDLE(rd_buffer_view, h)
#define rd_image_view_from_handle(h) RD_TEST_FROM_HANDLE(rd_image_view, h)
#define p_atomic_inc_return(v) __atomic_add_fetch((v), 1, __ATOMIC_ACQ_REL)
#define UNUSED __attribute__((unused))
static void rd_sampler_descriptor(const struct vk_sampler_state *state, uint32_t words[4])
{
   memcpy(words, state->words, sizeof(state->words));
}
static char descriptor_log[256];
static void mesa_loge(const char *format, const char *reason)
{
   snprintf(descriptor_log, sizeof(descriptor_log), format, reason);
}
#include "descriptor_updates_under_test.h"

/* Resource metadata shared with the shader compiler. */
#include "../radish/rd_nir_resources.h"
struct rd_pipeline {
   const struct ir3_shader_variant *variant;
   const struct vk_pipeline_layout *layout;
   const void *sampling_nir, *sampling_variants, *private_bo;
   bool batch_local_memory, linear_subgroups, linear_barriers;
   uint32_t descriptor_count, resource_count, buffer_count, sampling_mask;
   uint64_t used_descriptors;
   struct rd_nir_resource_binding resources[RD_MAX_ACTIVE_RESOURCES];
   struct rd_nir_resource_usage resource_usage;
   uint32_t texture_descriptors[16], sampler_descriptors[16], image_descriptors[16];
   uint8_t texture_planes[16];
   uint32_t buffer_descriptors[32];
};
struct rd_dispatch_record {
   const struct rd_pipeline *pipeline;
   struct rd_buffer *indirect;
   const struct rd_descriptor_set *sets[RD_MAX_SETS];
   uint32_t dynamic_offsets[RD_MAX_ACTIVE_RESOURCES];
};
#include "dispatch_preflight_under_test.h"

static void test_dispatch_preflight(void)
{
   unsigned gpu_id = 506, version = FD_VERSION_SOFTPIN;
   struct rd_device device = { .pipe = &gpu_id, .fd = &version, .info.threadsize_base = 32 };
   struct ir3_shader_variant variant = {
      .local_size = { 32, 1, 1 }, .num_ssbos = 2, .halfregs = 32,
      .shader_options.nir_options.a5xx_ssbo_size_consts = true,
   };
   struct rd_pipeline pipeline = {
      .variant = &variant, .batch_local_memory = true,
      .descriptor_count = 2, .resource_count = 2, .buffer_count = 2, .used_descriptors = 1,
      .resources = {
         { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },
         { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptor_set = 1 },
      },
   };
   struct rd_buffer buffer = { .vk.size = 256 };
   struct rd_set_layout layout = { .descriptor_count = 1 };
   struct rd_descriptor_set set = {
      .layout = &layout,
      .descriptors = {{ .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .buffer = &buffer,
                        .offset = 16, .range = 128 }},
   };
   struct rd_dispatch_record record = { .pipeline = &pipeline, .sets = { &set } };
   assert(rd_window_dispatch_supported(&device, &record)); /* unused set and BO not checked */
   version--;
   assert(!rd_window_dispatch_supported(&device, &record));
   version++;
   record.indirect = &buffer;
   assert(!rd_window_dispatch_supported(&device, &record));
   record.indirect = NULL;
   pipeline.batch_local_memory = false;
   assert(!rd_window_dispatch_supported(&device, &record));
   pipeline.batch_local_memory = true;
   pipeline.resources[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
   assert(!rd_window_dispatch_supported(&device, &record));
   pipeline.resources[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   pipeline.variant = NULL;
   assert(!rd_window_dispatch_supported(&device, &record));
   pipeline.variant = &variant;
   record.pipeline = NULL;
   assert(!rd_window_dispatch_supported(&device, &record));
   record.pipeline = &pipeline;
   pipeline.used_descriptors = 3;
   assert(!rd_window_dispatch_supported(&device, &record));
   pipeline.used_descriptors = 1;
   pipeline.resources[0].logical_slot = layout.descriptor_count;
   assert(!rd_window_dispatch_supported(&device, &record));
   pipeline.resources[0].logical_slot = 0;

   /* Check the live descriptor's type, and never modify the set in preflight. */
   set.descriptors[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
   record.dynamic_offsets[0] = 112;
   assert(rd_window_dispatch_supported(&device, &record));
   assert(set.descriptors[0].offset == 16);
   struct rd_descriptor snapshot[2] = {0};
   assert(rd_resolve_dispatch_descriptors(&record, snapshot));
   assert(snapshot[0].offset == 128 && snapshot[1].buffer == NULL);
   assert(set.descriptors[0].offset == 16);
   record.dynamic_offsets[0]++;
   assert(!rd_window_dispatch_supported(&device, &record));
   assert(!rd_resolve_dispatch_descriptors(&record, snapshot));
   record.dynamic_offsets[0] = 0;
   set.descriptors[0].buffer = NULL;
   assert(!rd_window_dispatch_supported(&device, &record));
}

#include "clear_ds_types_under_test.h"
#include "draw_descriptor_cache_types_under_test.h"
struct rd_command { enum rd_command_type type; struct rd_blit blit; };
struct rd_command_buffer {
   struct { VkCommandBufferLevel level; VkResult error; } vk;
   uint32_t push[RD_MAX_PUSH_CONSTANTS_SIZE / 4];
   uint32_t gfx_push[RD_MAX_PUSH_CONSTANTS_SIZE / 4];
   struct rd_render *render;
   bool render_continue;
   struct rd_command recorded[2];
   uint32_t recorded_count;
   const struct rd_pipeline *gfx_pipeline;
   uint64_t gfx_descriptor_bind_generation;
   struct rd_draw_descriptor_cache draw_descriptor_cache;
   struct rd_bind_state {
      struct rd_descriptor_set *sets[RD_MAX_SETS];
      uint32_t dynamic_offsets[RD_MAX_SETS][RD_MAX_BINDINGS];
   } bind[2];
};

struct vk_pipeline_layout {
   uint32_t set_count;
   const struct vk_descriptor_set_layout *set_layouts[RD_MAX_SETS];
   uint32_t push_range_count;
   VkPushConstantRange push_ranges[3];
};

typedef struct { unsigned sampler_index; } nir_tex_instr;
struct rd_resource_lower { struct rd_nir_resource_usage *usage; };
#include "sampler_slots_under_test.h"

static struct rd_command *rd_command_append(struct rd_command_buffer *command,
                                            enum rd_command_type type)
{
   assert(command->recorded_count < 2);
   struct rd_command *recorded = &command->recorded[command->recorded_count++];
   *recorded = (struct rd_command) { .type = type };
   return recorded;
}

#define rd_set_error(command_vk, result) ((command_vk)->error = (result))
#include "push_constants_under_test.h"
#include "clear_ds_under_test.h"
#undef rd_set_error

static void test_clear_ds_recording(void)
{
   const VkImageAspectFlags depth = VK_IMAGE_ASPECT_DEPTH_BIT;
   const VkImageAspectFlags stencil = VK_IMAGE_ASPECT_STENCIL_BIT;
   struct rd_image image = { .vk = {
      .aspects = depth | stencil, .mip_levels = 4, .array_layers = 8,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT, .stencil_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
   } };
   struct rd_command_buffer command = {0};
   VkCommandBuffer cmd = RD_TEST_HANDLE(VkCommandBuffer, &command);
   VkImage img = RD_TEST_HANDLE(VkImage, &image);
   VkClearDepthStencilValue value = { .depth = 0.5f, .stencil = 42 };
   const VkImageSubresourceRange valid[] = {
      { depth | stencil, 1, VK_REMAINING_MIP_LEVELS, 2, VK_REMAINING_ARRAY_LAYERS },
      { stencil, 3, 1, 7, 1 },
   };
   rd_CmdClearDepthStencilImage(cmd, img, VK_IMAGE_LAYOUT_GENERAL, &value, 2, valid);
   assert(command.vk.error == VK_SUCCESS && command.recorded_count == 2);
   assert(command.recorded[0].type == RD_CMD_BLIT);
   const struct rd_blit *blit = &command.recorded[0].blit;
   assert(blit->kind == RD_BLIT_CLEAR_DS && blit->dst == &image);
   assert(blit->dst_level == 1 && blit->levels == 3 && blit->dst_layer == 2 && blit->layers == 6);
   assert(blit->aspects == (depth | stencil) && blit->clear.depth == 0.5f && blit->clear.stencil == 42);
   blit = &command.recorded[1].blit;
   assert(blit->dst_level == 3 && blit->levels == 1 && blit->dst_layer == 7 && blit->layers == 1);

   const VkImageSubresourceRange invalid[] = {
      { depth, 4, VK_REMAINING_MIP_LEVELS, 0, 1 },
      { depth, 0, 1, UINT32_MAX, VK_REMAINING_ARRAY_LAYERS },
      { depth, 1, UINT32_MAX - 1, 0, 1 },
      { depth, 0, 1, 7, 2 },
      { depth, 0, 0, 0, 1 },
      { stencil, 0, 1, 0, 0 },
      { 0, 0, 1, 0, 1 },
      { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
   };
   command.recorded_count = 0;
   for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
      command.vk.error = VK_SUCCESS;
      rd_CmdClearDepthStencilImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &invalid[i]);
      assert(command.vk.error == VK_ERROR_UNKNOWN && !command.recorded_count);
   }

   image.vk.aspects = depth;
   assert(!rd_clear_ds_range_valid(&image.vk, &valid[1]));
   image.vk.aspects = depth | stencil;
   image.vk.usage = 0;
   assert(!rd_clear_ds_range_valid(&image.vk, &valid[0]));

   /* A stencil-only clear uses separate stencil usage, not depth usage. */
   command.vk.error = VK_SUCCESS;
   rd_CmdClearDepthStencilImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &valid[1]);
   assert(command.vk.error == VK_SUCCESS && command.recorded_count == 1);
   assert(command.recorded[0].blit.aspects == stencil);

   command.recorded_count = 0;
   rd_CmdClearDepthStencilImage(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, &value, 1, &valid[1]);
   assert(command.vk.error == VK_ERROR_UNKNOWN && !command.recorded_count);
   command.vk.error = VK_SUCCESS;
   command.render_continue = true;
   rd_CmdClearDepthStencilImage(cmd, img, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &valid[1]);
   assert(command.vk.error == VK_ERROR_UNKNOWN && !command.recorded_count);
}

static void test_push_constants(void)
{
   const VkShaderStageFlags graphics = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
   struct vk_pipeline_layout layout = {
      .push_range_count = 3,
      .push_ranges = {
         { .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 4, .size = 8 },
         { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = 12 },
         { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
           .offset = RD_MAX_PUSH_CONSTANTS_SIZE - 4, .size = 4 },
      },
   };
   struct rd_command_buffer command = {0};
   VkCommandBuffer cmd = RD_TEST_HANDLE(VkCommandBuffer, &command);
   VkPipelineLayout pipeline_layout = RD_TEST_HANDLE(VkPipelineLayout, &layout);
   const uint32_t values[] = { 5, 17, 23 };

   rd_CmdPushConstants(cmd, pipeline_layout, graphics, 4, 8, values);
   assert(command.vk.error == VK_SUCCESS);
   assert(!command.gfx_push[0] && command.gfx_push[1] == 5 && command.gfx_push[2] == 17);
   assert(!command.push[1] && !command.push[2]);

   rd_CmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                      RD_MAX_PUSH_CONSTANTS_SIZE - 4, 4, values);
   assert(command.vk.error == VK_SUCCESS);
   assert(command.push[RD_MAX_PUSH_CONSTANTS_SIZE / 4 - 1] == 5);
   assert(!command.gfx_push[RD_MAX_PUSH_CONSTANTS_SIZE / 4 - 1]);

   /* The vertex range covers all bytes, but the fragment range does not. */
   rd_CmdPushConstants(cmd, pipeline_layout, graphics, 0, 12, values);
   assert(command.vk.error == VK_ERROR_FEATURE_NOT_PRESENT);
   assert(!command.gfx_push[0] && command.gfx_push[1] == 5 && command.gfx_push[2] == 17);

   command.vk.error = VK_SUCCESS;
   rd_CmdPushConstants(cmd, pipeline_layout, graphics | VK_SHADER_STAGE_COMPUTE_BIT, 4, 8, values);
   assert(command.vk.error == VK_ERROR_FEATURE_NOT_PRESENT);
   assert(!command.push[1] && !command.push[2]);

   command.vk.error = VK_SUCCESS;
   rd_CmdPushConstants(cmd, pipeline_layout, graphics, 5, 4, values);
   assert(command.vk.error == VK_ERROR_FEATURE_NOT_PRESENT);
   assert(command.gfx_push[1] == 5 && command.gfx_push[2] == 17);
}

static bool draw_descriptor_cache_option = true;
static bool debug_get_option_draw_descriptor_cache(void)
{
   return draw_descriptor_cache_option;
}
#include "draw_descriptor_cache_under_test.h"

static void test_draw_descriptor_cache_enabled(void)
{
   struct rd_render render = { .descriptor_arena = true };
   struct rd_command_buffer command = {
      .vk.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .render = &render,
   };
   assert(rd_draw_descriptor_cache_enabled(&command, true));
   draw_descriptor_cache_option = false;
   assert(!rd_draw_descriptor_cache_enabled(&command, true));
   assert(render.descriptor_arena);
   draw_descriptor_cache_option = true;
   assert(rd_draw_descriptor_cache_enabled(&command, true));
   assert(!rd_draw_descriptor_cache_enabled(&command, false));
   render.descriptor_arena = false;
   assert(!rd_draw_descriptor_cache_enabled(&command, true));
   render.descriptor_arena = true;
   command.vk.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
   assert(!rd_draw_descriptor_cache_enabled(&command, true));
   command.vk.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   command.render = NULL;
   assert(!rd_draw_descriptor_cache_enabled(&command, true));
}

static void test_draw_descriptor_cache(void)
{
   struct rd_render render = { .descriptor_arena = true, .draws.size = sizeof(struct rd_draw) };
   struct rd_render next_render = render;
   struct rd_pipeline pipeline = {0}, other_pipeline = {0};
   struct rd_device device = {0};
   struct rd_descriptor_set first = {0}, last = {0};
   rd_descriptor_set_init_version(&device, &first);
   rd_descriptor_set_init_version(&device, &last);
   struct rd_command_buffer command = {
      .vk.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .render = &render,
      .gfx_pipeline = &pipeline, .gfx_descriptor_bind_generation = 3,
      .bind[1].sets = { [0] = &first, [RD_MAX_SETS - 1] = &last },
   };
   assert(!rd_draw_descriptor_cache_hit(&command));

   command.bind[1].dynamic_offsets[0][1] = 32;
   command.bind[1].dynamic_offsets[RD_MAX_SETS - 1][RD_MAX_BINDINGS - 1] = 64;
   command.draw_descriptor_cache = (struct rd_draw_descriptor_cache) {
       .render = &render, .pipeline = &pipeline, .bind_generation = 3,
       .draws_size = render.draws.size,
      .offsets = {
         { .count = 1, .entries = {{ .set = 0, .slot = 1, .value = 32 }} },
         { .count = 1, .entries = {{ .set = RD_MAX_SETS - 1, .slot = RD_MAX_BINDINGS - 1, .value = 64 }} },
      },
   };
   rd_draw_descriptor_cache_track_set(&command.draw_descriptor_cache, 0, &first);
   rd_draw_descriptor_cache_track_set(&command.draw_descriptor_cache, RD_MAX_SETS - 1, &last);
   assert(rd_draw_descriptor_cache_hit(&command));
   command.gfx_descriptor_bind_generation++;
   assert(!rd_draw_descriptor_cache_hit(&command));
   command.draw_descriptor_cache.bind_generation = command.gfx_descriptor_bind_generation;
   command.bind[1].dynamic_offsets[0][1]++;
   assert(!rd_draw_descriptor_cache_hit(&command));
   command.bind[1].dynamic_offsets[0][1]--;
   command.bind[1].dynamic_offsets[RD_MAX_SETS - 1][RD_MAX_BINDINGS - 1]++;
   assert(!rd_draw_descriptor_cache_hit(&command));
   command.bind[1].dynamic_offsets[RD_MAX_SETS - 1][RD_MAX_BINDINGS - 1]--;
   command.gfx_pipeline = &other_pipeline;
   assert(!rd_draw_descriptor_cache_hit(&command));
   command.gfx_pipeline = &pipeline;
   command.render = &next_render;
   assert(!rd_draw_descriptor_cache_hit(&command));
   command.render = &render;
   render.draws.size += sizeof(struct rd_draw);
   assert(!rd_draw_descriptor_cache_hit(&command));
   render.draws.size -= sizeof(struct rd_draw);
   command.bind[1].sets[0] = NULL;
   assert(!rd_draw_descriptor_cache_hit(&command));
   command.bind[1].sets[0] = &first;
   assert(rd_draw_descriptor_cache_hit(&command));

   /* Reset and secondary execution discard the entry even if the other key
    * fields would still match. */
   rd_invalidate_draw_descriptor_cache(&command);
   assert(!rd_draw_descriptor_cache_hit(&command));
   assert(!command.draw_descriptor_cache.set_mask);
}

static void test_draw_descriptor_set_generations(void)
{
   const VkDescriptorType combined = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   struct vk_sampler_state immutable = { .words = { 17 } };
   struct rd_set_layout src_layout = {
      .descriptor_count = 1, .binding_count = 1,
      .bindings = {{ .count = 1, .type = combined, .immutable_offset = UINT32_MAX }},
   };
   struct rd_set_layout dst_layout = src_layout;
   dst_layout.immutable_count = 1;
   dst_layout.immutable_sampler_states = &immutable;
   dst_layout.bindings[0].immutable_offset = 0;
   struct rd_device device = {0};
   struct rd_descriptor_set src = { .layout = &src_layout }, dst = { .layout = &dst_layout };
   rd_descriptor_set_init_version(&device, &src);
   rd_descriptor_set_init_version(&device, &dst);
   struct rd_render render = { .draws.size = sizeof(struct rd_draw) };
   struct rd_pipeline pipeline = {0}, other_pipeline = {0};
   struct rd_command_buffer a = {
      .render = &render, .gfx_pipeline = &pipeline,
      .bind[1].sets = { &src, &dst },
      .draw_descriptor_cache = { .render = &render, .pipeline = &pipeline,
                                 .draws_size = render.draws.size },
   };
   struct rd_command_buffer b = a;
   b.gfx_pipeline = b.draw_descriptor_cache.pipeline = &other_pipeline;
   rd_draw_descriptor_cache_track_set(&a.draw_descriptor_cache, 0, &src);
   rd_draw_descriptor_cache_track_set(&b.draw_descriptor_cache, 1, &dst);
   assert(rd_draw_descriptor_cache_hit(&a) && rd_draw_descriptor_cache_hit(&b));

   struct rd_descriptor_set unrelated;
   rd_descriptor_set_init_version(&device, &unrelated);
   assert(rd_draw_descriptor_cache_hit(&a) && rd_draw_descriptor_cache_hit(&b));
   /* Model a fresh allocation reusing dst's address after free or pool reset.
    * The generation starts at zero again, but the allocation serial differs. */
   rd_descriptor_set_init_version(&device, &dst);
   assert(!dst.version.generation && rd_draw_descriptor_cache_hit(&a));
   assert(!rd_draw_descriptor_cache_hit(&b));
   b.draw_descriptor_cache.set_mask = 0;
   rd_draw_descriptor_cache_track_set(&b.draw_descriptor_cache, 1, &dst);

   struct rd_image_view view = {0}, other_view = {0};
   struct rd_sampler sampler = { .state.words = { 5 } };
   VkDescriptorImageInfo image = {
      .imageView = RD_TEST_HANDLE(VkImageView, &view),
      .sampler = RD_TEST_HANDLE(VkSampler, &sampler),
   };
   VkWriteDescriptorSet write = {
      .dstSet = RD_TEST_HANDLE(VkDescriptorSet, &src), .descriptorCount = 1,
      .descriptorType = combined, .pImageInfo = &image,
   };
   rd_UpdateDescriptorSets(VK_NULL_HANDLE, 1, &write, 0, NULL);
   assert(src.descriptors[0].sampler[0] == 5);
   assert(!rd_draw_descriptor_cache_hit(&a) && rd_draw_descriptor_cache_hit(&b));
   a.draw_descriptor_cache.set_mask = 0;
   rd_draw_descriptor_cache_track_set(&a.draw_descriptor_cache, 0, &src);

   VkCopyDescriptorSet copy = {
      .srcSet = write.dstSet, .dstSet = RD_TEST_HANDLE(VkDescriptorSet, &dst),
      .descriptorCount = 1,
   };
   rd_UpdateDescriptorSets(VK_NULL_HANDLE, 0, NULL, 1, &copy);
   assert(dst.descriptors[0].image_view == &view && dst.descriptors[0].sampler[0] == 17);
   assert(rd_draw_descriptor_cache_hit(&a) && !rd_draw_descriptor_cache_hit(&b));
   b.draw_descriptor_cache.set_mask = 0;
   rd_draw_descriptor_cache_track_set(&b.draw_descriptor_cache, 1, &dst);

   struct vk_descriptor_update_template update = {
      .entry_count = 1, .entries = {{ .type = combined, .array_count = 1 }},
   };
   image.imageView = RD_TEST_HANDLE(VkImageView, &other_view);
   rd_UpdateDescriptorSetWithTemplate(VK_NULL_HANDLE, copy.dstSet,
      RD_TEST_HANDLE(VkDescriptorUpdateTemplate, &update), &image);
   assert(dst.descriptors[0].image_view == &other_view && dst.descriptors[0].sampler[0] == 17);
   assert(rd_draw_descriptor_cache_hit(&a) && !rd_draw_descriptor_cache_hit(&b));
}

struct rd_image_transfer {
   struct rd_image *src_image, *dst_image;
   struct rd_buffer *src_buffer, *dst_buffer;
};
#define vk_format_is_compressed(f) (0)
#define vk_format_is_depth_or_stencil(f) (0)
#include "copy_side_under_test.h"

static void test_copy_side(void)
{
   struct fd_bo buffer_bo = {0}, image_bo = {0};
   struct rd_memory buffer_mem = { .vk.size = 4096, .bo = &buffer_bo };
   struct rd_memory image_mem = { .vk.size = 8192, .bo = &image_bo };
   struct rd_image image = {
      .vk.format = VK_FORMAT_R8_UNORM, .vk.image_type = VK_IMAGE_TYPE_2D,
      .vk.samples = VK_SAMPLE_COUNT_1_BIT, .memory = &image_mem,
      .memory_offset = 512, .size = 4096,
   };
   struct rd_buffer buffer = { .vk.size = 2048, .memory = &buffer_mem, .offset = 128 };
   struct rd_image_transfer transfer = { .src_buffer = &buffer, .dst_image = &image };
   struct rd_memory *mem = NULL;
   VkDeviceSize offset = 0, size = 0;
   struct fd_bo *bos[2];
   assert(rd_image_copy_side(&transfer, true, &mem, &offset, &size));
   assert(mem == &buffer_mem && offset == 128 && size == 2048);
   assert(rd_image_copy_side(&transfer, false, &mem, &offset, &size));
   assert(mem == &image_mem && offset == 512 && size == 4096);
   rd_image_copy_bos(&transfer, bos);
   assert(bos[0] == &buffer_bo && bos[1] == &image_bo);

   transfer = (struct rd_image_transfer){ .src_image = &image, .dst_buffer = &buffer };
   assert(rd_image_copy_side(&transfer, true, &mem, &offset, &size));
   assert(mem == &image_mem && offset == 512 && size == 4096);
   assert(rd_image_copy_side(&transfer, false, &mem, &offset, &size));
   assert(mem == &buffer_mem && offset == 128 && size == 2048);
   rd_image_copy_bos(&transfer, bos);
   assert(bos[0] == &image_bo && bos[1] == &buffer_bo);

   image.native_layout.tile_mode = 1;
   assert(!rd_image_copy_side(&transfer, true, &mem, &offset, &size));
}

struct fd_cmd_sp { struct fd_bo *ring_bo; unsigned size; };
struct fd_ringbuffer_sp {
   struct fd_ringbuffer base;
   struct { unsigned short nr_cmds, max_cmds; struct fd_cmd_sp *cmds; } u;
   struct fd_bo *ring_bo;
};
#define to_fd_ringbuffer_sp(r) ((struct fd_ringbuffer_sp *)(r))
#define fd_bo_heap_block(bo) (bo)
#include "ring_backings_under_test.h"

static void test_ring_backings(void)
{
   struct fd_bo bo = { .handle = 1, .size = 4096 };
   struct fd_cmd_sp cmds[4] = {
      {&bo, 512}, {&bo, 256}, {&bo, 128}, {&bo, 64},
   };
   uint32_t words[256];
   struct fd_ringbuffer_sp ring = {
      .base = { .start = words, .cur = words + 64, .size = 1024 },
      .u = { .nr_cmds = 1, .cmds = cmds },
      .ring_bo = &bo,
   };
   struct fd_bo *bos[4];
   unsigned count = 0;
   uint64_t bytes = 0;
   assert(rd_ring_backings(&ring.base, bos, 4, &count, &bytes));
   assert(count == 1 && bytes == 768);
   ring.u.nr_cmds = 4;
   assert(rd_ring_backings(&ring.base, bos, 4, &count, &bytes));
   assert(count == 1 && bytes == 1216);
   struct fd_bo other = { .handle = 2, .size = 8192 };
   cmds[1].ring_bo = &other;
   assert(!rd_ring_backings(&ring.base, bos, 1, &count, &bytes));
}

#include "submit_diag_under_test.h"

static void test_render_query_before_work(void)
{
   struct rd_render render = {0};
   assert(!rd_render_query_before_work(&render));
   struct rd_render_query query = {0};
   render.queries.data = &query;
   render.queries.size = sizeof(query);
   assert(rd_render_query_before_work(&render));
   struct rd_draw draw = {0};
   render.draws.data = &draw;
   render.draws.size = sizeof(draw);
   assert(!rd_render_query_before_work(&render));
}

static void test_drain_index(void)
{
   assert(rd_drain_index("cpu") == RD_DRAIN_CPU);
   assert(rd_drain_index("final") == RD_DRAIN_FINAL);
   assert(rd_drain_index("capacity") == RD_DRAIN_CAPACITY);
   assert(rd_drain_index("signal-fallback") == RD_DRAIN_SIGNAL_FALLBACK);
   assert(rd_drain_index("unknown") == RD_DRAIN_OTHER);
}

static void test_submit_diag_commands(void)
{
   struct rd_submit_diag diag = { .completion_wait_ns = 90, .wait_export_ns = 30 };
   uint64_t before = rd_submit_diag_accounted_ns(&diag);
   assert(before == 90);

   diag.submit_ready_ns += 15;
   diag.completion_wait_ns += 25;
   diag.drain_ns[RD_DRAIN_CPU] += 10; /* Already in completion_wait_ns. */
   diag.oldest_wait_ns += 5;
   rd_submit_diag_end_commands(&diag, 100, before);
   assert(diag.cmd_wall_ns == 100 && diag.cmd_host_ns == 55);

   rd_submit_diag_end_commands(&diag, 20, rd_submit_diag_accounted_ns(&diag));
   assert(diag.cmd_wall_ns == 20 && diag.cmd_host_ns == 20);
   rd_submit_diag_end_commands(&diag, 40, before);
   assert(diag.cmd_wall_ns == 40 && diag.cmd_host_ns == 0);
}

static void test_descriptor_layout_bindings(void)
{
   struct rd_sampler sampler = {0};
   const VkSampler samplers[] = { RD_TEST_HANDLE(VkSampler, &sampler),
                                  RD_TEST_HANDLE(VkSampler, &sampler) };
   VkDescriptorSetLayoutBinding entries[] = {
      { .binding = 1u << 20, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 2, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = samplers },
      { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
      { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
   };
   VkDescriptorSetLayoutCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 3, .pBindings = entries,
   };
   uint32_t bindings, descriptors;
   assert(rd_descriptor_layout_supported(&info, &bindings, &descriptors));
   assert(bindings == 2 && descriptors == 3);

   /* Duplicate active bindings must not reach immutable sampler copying. */
   entries[2].binding = entries[0].binding;
   assert(!rd_descriptor_layout_supported(&info, &bindings, &descriptors));
   entries[2].pImmutableSamplers = samplers;
   assert(!rd_descriptor_layout_supported(&info, &bindings, &descriptors));

   info.pBindings = NULL;
   assert(!rd_descriptor_layout_supported(&info, &bindings, &descriptors));
   info.bindingCount = 0;
   assert(rd_descriptor_layout_supported(&info, &bindings, &descriptors));
   assert(bindings == 0 && descriptors == 0);
}

static void test_descriptor_span(void)
{
   struct rd_set_layout layout = {
      .descriptor_count = 6,
      .binding_count = 3,
      .bindings = {
         { .slot = 0, .count = 2, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE },
         { .slot = 2, .count = 2, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
         { .slot = 4, .count = 2, .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
      },
   };
   assert(rd_descriptor_span_valid(&layout, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2));
   assert(rd_descriptor_span_valid(&layout, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1));
   assert(!rd_descriptor_span_valid(&layout, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2));
   assert(rd_descriptor_span_valid(&layout, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2));
   assert(!rd_descriptor_span_valid(&layout, 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3));
   assert(!rd_descriptor_span_valid(&layout, 7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0));
   assert(rd_descriptor_span_valid(&layout, 6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0));
}

static void test_ycbcr_formats(void)
{
   const struct {
      VkFormat format, luma, chroma;
      unsigned planes, chroma_width, chroma_height;
   } formats[] = {
      {VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, 2, 9, 7},
      {VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, 3, 9, 7},
      {VK_FORMAT_G8_B8R8_2PLANE_422_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, 2, 9, 14},
      {VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, 3, 9, 14},
      {VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, 3, 18, 14},
      {VK_FORMAT_G16_B16R16_2PLANE_420_UNORM, VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, 2, 9, 7},
      {VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, 3, 9, 7},
      {VK_FORMAT_G16_B16R16_2PLANE_422_UNORM, VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, 2, 9, 14},
      {VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, 3, 9, 14},
      {VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, 3, 18, 14},
   };
   for (unsigned i = 0; i < ARRAY_SIZE(formats); i++) {
      VkFormat format = formats[i].format;
      const struct rd_image image = { .vk = { .format = format, .extent = {18, 14, 1} } };
      assert(rd_ycbcr_format_supported(format));
      assert(vk_format_get_plane_count(format) == formats[i].planes);
      const struct vk_format_ycbcr_info *info = vk_format_get_ycbcr_info(format);
      assert(info->planes[0].ycbcr_swizzle[0] == VK_COMPONENT_SWIZZLE_G);
      assert(info->planes[1].ycbcr_swizzle[0] == VK_COMPONENT_SWIZZLE_B);
      assert(formats[i].planes == 2 ? info->planes[1].ycbcr_swizzle[1] == VK_COMPONENT_SWIZZLE_R :
                                    info->planes[2].ycbcr_swizzle[0] == VK_COMPONENT_SWIZZLE_R);
      for (unsigned p = 0; p < formats[i].planes; p++) {
         assert(rd_image_aspect_plane(format, VK_IMAGE_ASPECT_PLANE_0_BIT << p) == p);
         assert(vk_format_get_plane_format(format, p) == (p ? formats[i].chroma : formats[i].luma));
         VkExtent3D extent = rd_image_plane_extent(&image, p, 0);
         assert(extent.width == (p ? formats[i].chroma_width : 18));
         assert(extent.height == (p ? formats[i].chroma_height : 14));
         assert(extent.depth == 1);
      }
      if (formats[i].planes == 2)
         assert(rd_image_aspect_plane(format, VK_IMAGE_ASPECT_PLANE_2_BIT) == UINT32_MAX);
   }

   assert(!rd_ycbcr_format_supported(VK_FORMAT_R16_UNORM));
   assert(!rd_ycbcr_format_supported(VK_FORMAT_G8B8G8R8_422_UNORM));
   assert(!rd_ycbcr_format_supported(VK_FORMAT_G16B16G16R16_422_UNORM));
   assert(!rd_ycbcr_format_supported(VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16));
   assert(!rd_ycbcr_format_supported(VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16));
   assert(!rd_ycbcr_format_supported(VK_FORMAT_G8_B8R8_2PLANE_444_UNORM));
   assert(!rd_ycbcr_format_supported(VK_FORMAT_G16_B16R16_2PLANE_444_UNORM));
}

static void test_ycbcr_sampler_slots(void)
{
   struct vk_sampler_state samplers[] = {
      { .has_ycbcr_conversion = true, .ycbcr_conversion.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM },
      { .has_ycbcr_conversion = true, .ycbcr_conversion.format = VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM },
   };
   struct rd_set_layout set = {
      .binding_count = 4, .descriptor_count = 29,
      .immutable_count = ARRAY_SIZE(samplers), .immutable_sampler_states = samplers,
      .bindings = {
         { .binding = 0, .count = 12, .slot = 0, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
           .immutable_offset = UINT32_MAX },
         { .binding = 1, .count = 14, .slot = 12, .type = VK_DESCRIPTOR_TYPE_SAMPLER,
           .immutable_offset = UINT32_MAX },
         { .binding = 2, .count = 2, .slot = 26, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
           .immutable_offset = 0 },
         { .binding = 3, .count = 1, .slot = 28, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
           .immutable_offset = UINT32_MAX },
      },
   };
   const struct vk_pipeline_layout layout = { .set_count = 1, .set_layouts = { &set.vk } };
   struct rd_pipeline pipeline = { .layout = &layout };
   struct rd_resource_lower lower = { .usage = &pipeline.resource_usage };
   nir_tex_instr tex = {0};
   const struct rd_nir_resource_binding *plain = rd_pipeline_resource(&pipeline, 0, 3, 0);
   assert(plain && plain->texture_slot == 0 && plain->sampler_slot == 0 && plain->plane_count == 1);
   assert(rd_lower_texture_sampler(&tex, &lower, plain, 0, 0) && tex.sampler_index == 0);
   assert(!rd_lower_texture_sampler(&tex, &lower, plain, 0, 1));

   const struct rd_nir_resource_binding *two = rd_pipeline_resource(&pipeline, 0, 2, 0);
   assert(two && two->texture_slot == 1 && two->sampler_slot == 1 && two->plane_count == 2);
   const struct rd_nir_resource_binding *separate = rd_pipeline_resource(&pipeline, 0, 1, 0);
   assert(separate && separate->texture_slot == UINT32_MAX && separate->sampler_slot == 2);
   assert(rd_lower_texture_sampler(&tex, &lower, separate, 0, 0) && tex.sampler_index == 2);
   const struct rd_nir_resource_binding *three = rd_pipeline_resource(&pipeline, 0, 2, 1);
   assert(three && three->texture_slot == 3 && three->sampler_slot == 3 && three->plane_count == 3);

   const struct rd_nir_resource_binding *converted[] = {two, three};
   for (unsigned i = 0; i < ARRAY_SIZE(converted); i++) {
      const struct rd_nir_resource_binding *entry = converted[i];
      assert(pipeline.sampler_descriptors[entry->sampler_slot] == entry->descriptor_slot);
      for (uint32_t p = 0; p < entry->plane_count; p++) {
         assert(pipeline.texture_descriptors[entry->texture_slot + p] == entry->descriptor_slot);
         assert(pipeline.texture_planes[entry->texture_slot + p] == p);
         assert(rd_lower_texture_sampler(&tex, &lower, entry, 0, p));
         assert(tex.sampler_index == entry->sampler_slot);
      }
      assert(!rd_lower_texture_sampler(&tex, &lower, entry, 0, entry->plane_count));
   }
   assert(pipeline.resource_usage.samplers == 0xf && pipeline.resource_usage.sampler_count == 4);
   assert(pipeline.resource_usage.descriptors == 0xf);

   for (uint32_t i = 0; i < 10; i++) {
      const struct rd_nir_resource_binding *entry = rd_pipeline_resource(&pipeline, 0, 0, i);
      assert(entry && entry->texture_slot == 6 + i && entry->sampler_slot == UINT32_MAX);
   }
   assert(!rd_pipeline_resource(&pipeline, 0, 0, 10));
   for (uint32_t i = 1; i < 13; i++) {
      const struct rd_nir_resource_binding *entry = rd_pipeline_resource(&pipeline, 0, 1, i);
      assert(entry && entry->sampler_slot == 3 + i);
   }
   assert(!rd_pipeline_resource(&pipeline, 0, 1, 13));
   assert(rd_pipeline_resource(&pipeline, 0, 2, 1) == three);
}

enum a4xx_state_block { SB4_FS_TEX = 4, SB4_CS_TEX = 5 };
enum a4xx_state_type { ST4_CONSTANTS = 1 };

/* Packet fields for the host test; the driver uses generated A5xx headers. */
#define COND(c, v) ((c) ? (v) : 0)
#define CP_LOAD_STATE4 0x30u
#define SS4_DIRECT 0u
#define CP_LOAD_STATE4_0_DST_OFF(v) (v)
#define CP_LOAD_STATE4_0_STATE_SRC(v) ((v) << 16)
#define CP_LOAD_STATE4_0_STATE_BLOCK(v) ((uint32_t)(v) << 18)
#define CP_LOAD_STATE4_0_NUM_UNIT(v) ((v) << 22)
#define CP_LOAD_STATE4_1_STATE_TYPE(v) (v)
#define A5XX_TEX_CONST_0_MIPLVLS(v) ((v) << 16)
#define A5XX_TEX_CONST_1_WIDTH(v) (v)
#define A5XX_TEX_CONST_1_HEIGHT(v) ((v) << 15)
#define A5XX_TEX_CONST_2_BUFFER (1u << 4)
#define A5XX_TEX_CONST_2_TYPE(v) ((uint32_t)(v) << 29)
#define A5XX_TEX_CONST_2_PITCHALIGN(v) (v)
#define A5XX_TEX_CONST_2_PITCH(v) ((v) << 7)
#define A5XX_TEX_CONST_3_ARRAY_PITCH(v) ((v) >> 12)
#define A5XX_TEX_CONST_3_MIN_LAYERSZ(v) (((v) >> 12) << 23)
#define A5XX_TEX_CONST_5_DEPTH(v) ((v) << 17)

static void OUT_RING(struct fd_ringbuffer *ring, uint32_t word)
{
   assert(ring->cur < ring->end);
   *ring->cur++ = word;
}

static void OUT_PKT7(struct fd_ringbuffer *ring, uint32_t opcode, uint32_t dwords)
{
   assert((size_t)(ring->end - ring->cur) >= dwords + 1u);
   OUT_RING(ring, (opcode << 16) | dwords); /* Parity is handled by Freedreno. */
}

static void OUT_RELOC(struct fd_ringbuffer *ring, struct fd_bo *bo, uint32_t offset,
                      uint64_t orval, int32_t shift)
{
   assert(bo == &border_bo && !shift);
   uint64_t address = (fd_bo_get_iova(bo) + offset) | orval;
   OUT_RING(ring, (uint32_t)address);
   OUT_RING(ring, address >> 32);
}

static bool rd_make_resource(const struct rd_descriptor *desc, struct rd_resource *resource,
                             uint32_t plane)
{
   if (!desc->image_view || plane >= vk_format_get_plane_count(desc->image_view->conversion.format))
      return false;
   *resource = (struct rd_resource) {
      .bo = &border_bo, .offset = 64 + 4096 * plane, .type = A5XX_TEX_2D,
      .width = 18 - 4 * plane, .height = 14, .depth = 2, .row_pitch = 128,
      .array_pitch = 4096 * (plane + 1), .format_word = (0x15u + plane) << 22,
   };
   return true;
}
#include "sampled_textures_under_test.h"

static const uint32_t *check_texture_packet(const uint32_t *words, enum a4xx_state_block sb,
                                            unsigned slot, unsigned count, unsigned first_plane,
                                            uint32_t size_only_mask)
{
   assert(words[0] == ((CP_LOAD_STATE4 << 16) | (3 + 12 * count)));
   assert(words[1] == (((uint32_t)sb << 18) | (count << 22) | slot));
   assert(words[2] == 1 && words[3] == 0);
   words += 4;
   for (unsigned i = 0; i < count; i++, words += 12) {
      unsigned plane = first_plane + i;
      assert(words[0] == (0x15u + plane) << 22);
      assert(words[1] == ((14u << 15) | (18 - 4 * plane)));
      assert(words[2] == ((1u << 29) | (128u << 7)));
      assert(words[3] == ((size_only_mask & (1u << (slot + i))) ? 0 : plane + 1));
      assert(words[4] == 8192 + 64 + 4096 * plane && words[5] == 2u << 17);
      for (unsigned j = 6; j < 12; j++)
         assert(words[j] == 0);
   }
   return words;
}

static void test_ycbcr_texture_packets(void)
{
   struct rd_image_view three = { .ycbcr = true, .conversion.format = VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM };
   struct rd_image_view two = { .ycbcr = true, .conversion.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM };
   struct rd_image_view plain = { .conversion.format = VK_FORMAT_R8_UNORM };
   const struct rd_descriptor descriptors[] = {
      { .image_view = &three }, { .image_view = &two }, { .image_view = &plain },
   };
   struct rd_pipeline pipeline = {
      .resource_usage = { .textures = 0x3f, .texture_count = 6 },
      .texture_descriptors = {0, 0, 0, 1, 1, 2},
      .texture_planes = {0, 1, 2, 0, 1, 0},
   };
   uint32_t words[6 * 16];
   struct fd_ringbuffer ring = { .start = words, .cur = words, .end = words + ARRAY_SIZE(words) };

   rd_emit_sampled_textures(&ring, SB4_FS_TEX, &pipeline, descriptors);
   const uint32_t *next = check_texture_packet(words, SB4_FS_TEX, 0, 3, 0, 0);
   next = check_texture_packet(next, SB4_FS_TEX, 3, 2, 0, 0);
   assert(ring.cur == check_texture_packet(next, SB4_FS_TEX, 5, 1, 0, 0));

   struct rd_resource resource;
   assert(rd_make_resource(&descriptors[2], &resource, 0));
   uint32_t single[16];
   struct fd_ringbuffer single_ring = { .start = single, .cur = single, .end = single + 16 };
   rd_emit_resource_texture_sb(&single_ring, 5, &resource, SB4_FS_TEX);
   assert(single_ring.cur == single + 16 && !memcmp(next, single, sizeof(single)));

   /* Inactive plane 1 breaks the run; the software size query on plane 2
    * still gets a descriptor, but without the native array pitch. */
   pipeline.resource_usage.textures &= ~(1u << 1);
   pipeline.sampling_mask = 1u << 2;
   ring.cur = words;
   rd_emit_sampled_textures(&ring, SB4_CS_TEX, &pipeline, descriptors);
   next = check_texture_packet(words, SB4_CS_TEX, 0, 1, 0, 0);
   next = check_texture_packet(next, SB4_CS_TEX, 2, 1, 2, pipeline.sampling_mask);
   next = check_texture_packet(next, SB4_CS_TEX, 3, 2, 0, 0);
   assert(ring.cur == check_texture_packet(next, SB4_CS_TEX, 5, 1, 0, 0));

   /* A failed plane must not be counted in a packet or suppress a later slot. */
   pipeline.resource_usage.textures = 0x07;
   pipeline.texture_planes[1] = 3;
   ring.cur = words;
   rd_emit_sampled_textures(&ring, SB4_CS_TEX, &pipeline, descriptors);
   next = check_texture_packet(words, SB4_CS_TEX, 0, 1, 0, 0);
   assert(ring.cur == check_texture_packet(next, SB4_CS_TEX, 2, 1, 2, pipeline.sampling_mask));
}

static void test_descriptor_ycbcr_conversion(void)
{
   const VkDescriptorType type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   struct rd_image_view ycbcr_view = {
      .ycbcr = true,
      .conversion = {
         .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
         .ycbcr_model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601,
         .ycbcr_range = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
         .mapping = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                      VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A },
         .chroma_offsets = { VK_CHROMA_LOCATION_COSITED_EVEN, VK_CHROMA_LOCATION_MIDPOINT },
         .chroma_filter = VK_FILTER_LINEAR,
         .chroma_reconstruction = true,
      },
   };
   struct rd_image_view plain_view = {0};
   struct rd_sampler ycbcr_sampler = { .state = {
      .has_ycbcr_conversion = true, .ycbcr_conversion = ycbcr_view.conversion, .words = { 17 },
   } };
   struct rd_sampler plain_sampler = { .state.words = { 5 } };
   struct vk_sampler_state immutable = ycbcr_sampler.state;
   struct rd_set_layout mutable_layout = {
      .descriptor_count = 1, .binding_count = 1,
      .bindings = {{ .count = 1, .type = type, .immutable_offset = UINT32_MAX }},
   };
   struct rd_set_layout immutable_layout = mutable_layout;
   immutable_layout.immutable_count = 1;
   immutable_layout.immutable_sampler_states = &immutable;
   immutable_layout.bindings[0].immutable_offset = 0;
   struct rd_descriptor_set dst = { .layout = &mutable_layout };
   struct rd_descriptor *desc = &dst.descriptors[0];
   VkDescriptorImageInfo image = {
      .imageView = RD_TEST_HANDLE(VkImageView, &plain_view),
      .sampler = RD_TEST_HANDLE(VkSampler, &plain_sampler),
   };
   VkWriteDescriptorSet write = {
      .dstSet = RD_TEST_HANDLE(VkDescriptorSet, &dst), .descriptorCount = 1,
      .descriptorType = type, .pImageInfo = &image,
   };

   /* An ordinary mutable combined descriptor is unchanged. Replacing it
    * with a converted sampler and an unconverted view must invalidate it. */
   rd_UpdateDescriptorSets(VK_NULL_HANDLE, 1, &write, 0, NULL);
   assert(desc->image_view == &plain_view && desc->has_sampler && desc->sampler[0] == 5);
   image.sampler = RD_TEST_HANDLE(VkSampler, &ycbcr_sampler);
   rd_UpdateDescriptorSets(VK_NULL_HANDLE, 1, &write, 0, NULL);
   assert(!desc->image_view && !desc->has_sampler && dst.version.generation == 2);
   assert(strstr(descriptor_log, "conversions do not match"));

   /* Matching values do not make a converted mutable binding valid. */
   image.imageView = RD_TEST_HANDLE(VkImageView, &ycbcr_view);
   rd_UpdateDescriptorSets(VK_NULL_HANDLE, 1, &write, 0, NULL);
   assert(!desc->image_view && !desc->has_sampler);
   assert(strstr(descriptor_log, "requires an immutable sampler"));
   ycbcr_sampler.state.ycbcr_conversion.mapping[0] = VK_COMPONENT_SWIZZLE_B;
   ycbcr_sampler.state.ycbcr_conversion.mapping[2] = VK_COMPONENT_SWIZZLE_R;
   rd_UpdateDescriptorSets(VK_NULL_HANDLE, 1, &write, 0, NULL);
   assert(!desc->image_view && !desc->has_sampler);
   assert(strstr(descriptor_log, "conversions do not match"));

   /* An immutable sampler uses the layout state, not the ignored handle
    * in the write. An independently stored, equal conversion is accepted. */
   struct rd_descriptor_set src = { .layout = &immutable_layout };
   write.dstSet = RD_TEST_HANDLE(VkDescriptorSet, &src);
   descriptor_log[0] = '\0';
   rd_UpdateDescriptorSets(VK_NULL_HANDLE, 1, &write, 0, NULL);
   assert(src.descriptors[0].image_view == &ycbcr_view && src.descriptors[0].has_sampler);
   assert(src.descriptors[0].sampler[0] == 17 && !descriptor_log[0]);

   struct vk_descriptor_update_template update = {
      .entry_count = 1, .entries = {{ .type = type, .array_count = 1 }},
   };
   VkDescriptorUpdateTemplate update_handle = RD_TEST_HANDLE(VkDescriptorUpdateTemplate, &update);
   struct rd_image_view wrong_view = ycbcr_view;
   wrong_view.conversion.ycbcr_model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
   image.imageView = RD_TEST_HANDLE(VkImageView, &wrong_view);
   rd_UpdateDescriptorSetWithTemplate(VK_NULL_HANDLE, write.dstSet, update_handle, &image);
   assert(!src.descriptors[0].image_view && !src.descriptors[0].has_sampler);
   assert(strstr(descriptor_log, "conversions do not match"));
   image.imageView = RD_TEST_HANDLE(VkImageView, &ycbcr_view);
   rd_UpdateDescriptorSetWithTemplate(VK_NULL_HANDLE, write.dstSet, update_handle, &image);
   assert(src.descriptors[0].image_view == &ycbcr_view && src.descriptors[0].has_sampler);

   /* Copying a converted descriptor must not bypass the destination
    * binding's immutable requirement or its conversion comparison. */
   VkCopyDescriptorSet copy = {
      .srcSet = write.dstSet, .dstSet = RD_TEST_HANDLE(VkDescriptorSet, &dst), .descriptorCount = 1,
   };
   rd_UpdateDescriptorSets(VK_NULL_HANDLE, 0, NULL, 1, &copy);
   assert(!desc->image_view && !desc->has_sampler);

   struct vk_sampler_state dst_sampler = immutable;
   dst_sampler.words[0] = 29;
   struct rd_set_layout dst_layout = immutable_layout;
   dst_layout.immutable_sampler_states = &dst_sampler;
   dst.layout = &dst_layout;
   rd_UpdateDescriptorSets(VK_NULL_HANDLE, 0, NULL, 1, &copy);
   assert(desc->image_view == &ycbcr_view && desc->has_sampler && desc->sampler[0] == 29);
   dst_sampler.ycbcr_conversion.chroma_filter = VK_FILTER_NEAREST;
   rd_UpdateDescriptorSets(VK_NULL_HANDLE, 0, NULL, 1, &copy);
   assert(!desc->image_view && !desc->has_sampler);
   assert(strstr(descriptor_log, "conversions do not match"));

   /* The converted sampler cannot be used through a sampler-only binding. */
   mutable_layout.bindings[0].type = write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
   dst.layout = &mutable_layout;
   write.dstSet = copy.dstSet;
   rd_UpdateDescriptorSets(VK_NULL_HANDLE, 1, &write, 0, NULL);
   assert(!desc->has_sampler);
   assert(strstr(descriptor_log, "combined image sampler binding"));
}

static void test_descriptor_template_data(void)
{
   const VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
   const size_t info_size = sizeof(VkDescriptorBufferInfo);
   struct vk_descriptor_update_template update = {
      .entry_count = 1, .entries = {{ .type = type, .array_count = 2,
                                     .offset = 1, .stride = info_size + 3 }},
   };
   struct vk_descriptor_template_entry *entry = &update.entries[0];
   struct rd_set_layout layout = {
      .descriptor_count = 2, .binding_count = 1,
      .bindings = {{ .count = 2, .type = type, .immutable_offset = UINT32_MAX }},
   };
   struct rd_descriptor_set set = { .layout = &layout };
   struct rd_buffer buffer = { .vk.size = 128 };
   VkDescriptorBufferInfo info = {
      .buffer = RD_TEST_HANDLE(VkBuffer, &buffer), .offset = 16, .range = 32,
   };
   uint8_t data[1 + 2 * sizeof(VkDescriptorBufferInfo) + 3];
   memcpy(data + entry->offset, &info, info_size);
   info.offset = 64;
   memcpy(data + entry->offset + entry->stride, &info, info_size);

   VkDescriptorSet set_handle = RD_TEST_HANDLE(VkDescriptorSet, &set);
   VkDescriptorUpdateTemplate update_handle = RD_TEST_HANDLE(VkDescriptorUpdateTemplate, &update);
   rd_UpdateDescriptorSetWithTemplate(VK_NULL_HANDLE, set_handle, update_handle, data);
   assert(set.descriptors[0].buffer == &buffer && set.descriptors[0].offset == 16);
   assert(set.descriptors[1].buffer == &buffer && set.descriptors[1].offset == 64);
   assert(set.version.generation == 1);

   /* Reject the entire entry if its last element would overflow. */
   info.offset = 96;
   memcpy(data + entry->offset, &info, info_size);
   entry->stride = SIZE_MAX;
   rd_UpdateDescriptorSetWithTemplate(VK_NULL_HANDLE, set_handle, update_handle, data);
   assert(set.descriptors[0].offset == 16 && set.descriptors[1].offset == 64);
   assert(set.version.generation == 1);

   entry->array_count = 3;
   entry->offset = 0;
   assert(!rd_descriptor_template_data_valid(entry, 0)); /* stride product */
   entry->array_count = 1;
   entry->offset = SIZE_MAX - info_size + 1;
   assert(!rd_descriptor_template_data_valid(entry, 0)); /* last element's read */
   entry->offset = 0;
   assert(rd_descriptor_template_data_valid(entry, UINTPTR_MAX - info_size));
   assert(!rd_descriptor_template_data_valid(entry, UINTPTR_MAX - info_size + 1));

   /* A single element ignores stride; a zero stride reuses an element. */
   entry->offset = 1;
   entry->array_element = 1;
   rd_UpdateDescriptorSetWithTemplate(VK_NULL_HANDLE, set_handle, update_handle, data);
   assert(set.descriptors[1].offset == 96 && set.version.generation == 2);
   entry->array_count = 2;
   entry->array_element = 0;
   entry->stride = 0;
   rd_UpdateDescriptorSetWithTemplate(VK_NULL_HANDLE, set_handle, update_handle, data);
   assert(set.descriptors[0].offset == 96 && set.descriptors[1].offset == 96);
   assert(set.version.generation == 3);
}

static void test_query_range(void)
{
   struct rd_query_pool pool = { .vk.query_count = 4 };
   assert(rd_query_range(&pool, 0, 1));
   assert(rd_query_range(&pool, 3, 1));
   assert(!rd_query_range(&pool, 4, 1));
   assert(!rd_query_range(&pool, 3, 2));
   assert(!rd_query_range(NULL, 0, 1));
}

static void test_vertex_input(void)
{
   assert(rd_vertex_input_valid(NULL));
   VkVertexInputBindingDescription binding = {
      .binding = 0, .stride = 16, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
   };
   VkVertexInputAttributeDescription attribute = {
      .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0,
   };
   VkPipelineVertexInputStateCreateInfo vi = {
      .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding,
      .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &attribute,
   };
   assert(rd_vertex_input_valid(&vi));
   binding.binding = 32;
   assert(!rd_vertex_input_valid(&vi));
   binding.binding = 0;
   attribute.binding = 32;
   assert(!rd_vertex_input_valid(&vi));
   attribute.binding = 1;
   assert(!rd_vertex_input_valid(&vi));
   attribute.binding = 0;
   attribute.location = 32;
   assert(!rd_vertex_input_valid(&vi));
   attribute.location = 0;
   binding.stride = UINT16_MAX + 1;
   assert(!rd_vertex_input_valid(&vi));
   binding.stride = 16;
   binding.inputRate = 2;
   assert(!rd_vertex_input_valid(&vi));
   binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
   attribute.offset = 2048;
   assert(!rd_vertex_input_valid(&vi));
}

static void test_fence_export(void)
{
   struct rd_render render = { .layers = 1 };
   assert(rd_render_exports_fence(&render));
   render.view_mask = 1;
   assert(!rd_render_exports_fence(&render));
   render.view_mask = 0;
   render.layers = 2;
   assert(!rd_render_exports_fence(&render));
   render.layers = 1;
   render.depth.view = (void *)1;
   assert(!rd_render_exports_fence(&render));
   render.depth.view = NULL;
   render.stencil.view = (void *)1;
   assert(!rd_render_exports_fence(&render));
   render.stencil.view = NULL;
   render.color_count = 2;
   render.color[1].resolve = (void *)1;
   assert(!rd_render_exports_fence(&render));
   render.color[1].resolve = NULL;
   assert(rd_render_exports_fence(&render));
}

static void test_sync_policy(void)
{
   assert(rd_compute_can_defer(true, false, false, true));
   assert(!rd_compute_can_defer(true, false, true, true));
   assert(!rd_compute_can_defer(true, false, false, false));
   assert(rd_compute_can_defer(false, true, false, true));
   assert(!rd_compute_can_defer(false, false, false, true));
   assert(rd_publish_signal_fences(true, false, false, true, 1));
   assert(!rd_publish_signal_fences(false, false, false, true, 1));
   assert(!rd_publish_signal_fences(true, true, false, true, 1));
   assert(!rd_publish_signal_fences(true, false, true, true, 1));
   assert(!rd_publish_signal_fences(true, false, false, false, 1));
   assert(!rd_publish_signal_fences(true, false, false, true, 0));
}

static void test_compute_tail_batch(void)
{
   const uint32_t dispatch_grid[3] = {9, 7, 5};
   const uint32_t base[3] = {8, 4, 4};
   const uint32_t grid[3] = {1, 3, 1};
   assert(rd_compute_tail_batch(true, base, grid, dispatch_grid));
   assert(!rd_compute_tail_batch(false, base, grid, dispatch_grid));
   assert(!rd_compute_tail_batch(true, (uint32_t[]){4, 4, 4}, grid, dispatch_grid));
   assert(!rd_compute_tail_batch(true, (uint32_t[]){8, 0, 4}, grid, dispatch_grid));
   assert(!rd_compute_tail_batch(true, (uint32_t[]){8, 4, 0}, grid, dispatch_grid));
   assert(rd_compute_tail_batch(true, (uint32_t[]){0, 0, 0}, dispatch_grid, dispatch_grid));
}

static void test_sync_waits(void)
{
   struct vk_sync binary = {0}, timeline = {VK_SYNC_IS_TIMELINE};
   struct vk_sync_wait binary_first[] = {{&binary}, {&timeline}};
   struct vk_sync_wait timeline_first[] = {{&timeline}, {&binary}};
   assert(vk_sync_waits_have_timeline(2, binary_first));
   assert(vk_sync_waits_have_timeline(2, timeline_first));
   assert(!vk_sync_waits_have_timeline(1, binary_first));
}

static void test_image_pitch(void)
{
   struct rd_image image = {0};
   image.levels[0].array_pitch = 128ull << 20;
   assert(rd_image_sampled_pitch_supported(&image));
   image.vk.array_layers = 2;
   assert(rd_image_sampled_pitch_supported(&image));
   image.vk.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
   assert(!rd_image_sampled_pitch_supported(&image));
   image.levels[0].array_pitch = 4096;
   assert(rd_image_sampled_pitch_supported(&image));
   image.levels[0].array_pitch = 4097;
   assert(!rd_image_sampled_pitch_supported(&image));
}

static void test_device_extensions(void)
{
   struct vk_device_extension_table extensions = {true, true};
   rd_device_extensions_for_mode(&extensions, true);
   assert(extensions.KHR_swapchain && extensions.KHR_incremental_present);
   rd_device_extensions_for_mode(&extensions, false);
   assert(!extensions.KHR_swapchain && !extensions.KHR_incremental_present);
}

static void test_blit_src(void)
{
   assert(!rd_image_required_blit_src(VK_FORMAT_D16_UNORM));
   assert(!rd_image_required_blit_src(VK_FORMAT_D32_SFLOAT));
   assert(rd_image_required_blit_src(VK_FORMAT_R8G8B8A8_UNORM));
}

static void test_subword(void)
{
   struct nir_mem_access_size_align r8 =
      rd_subword_storage_access(0, 1, 8, 1, 0, false, RD_ACCESS_NONE, NULL);
   assert(r8.num_components == 1 && r8.bit_size == 32 && r8.align == 4);
   struct nir_mem_access_size_align r16 =
      rd_subword_storage_access(0, 2, 16, 2, 0, false, RD_ACCESS_NONE, NULL);
   assert(r16.num_components == 1 && r16.bit_size == 32 && r16.align == 4);
   struct nir_mem_access_size_align r32 =
      rd_subword_storage_access(0, 4, 32, 4, 0, false, RD_ACCESS_NONE, NULL);
   assert(r32.num_components == 1 && r32.bit_size == 32 && r32.align == 1);
}

static void test_growth(void)
{
   struct util_dynarray queries = {0}, draws = {0};
   struct rd_render_query query = {.query = 3};
   struct rd_draw draw = {.vertex_count = 9};
   fail_growth = true;
   assert(!rd_render_append_query(&queries, &query));
   assert(!rd_render_append_meta_draw(&draws, &draw));
   assert(!queries.data && !queries.size && !draws.data && !draws.size);
   fail_growth = false;
   assert(rd_render_append_query(&queries, &query));
   assert(rd_render_append_meta_draw(&draws, &draw));
   assert(((struct rd_render_query *)queries.data)->query == 3);
   assert(((struct rd_draw *)draws.data)->vertex_count == 9);
   fail_growth = true;
   assert(!rd_render_append_query(&queries, &query));
   assert(queries.size == sizeof(query));
   fail_growth = false;
   free(queries.data); free(draws.data);
}

static void test_restart(void)
{
   const uint16_t indices16[] = {0xffff, 0, 1, 2, 0xffff, 0xffff, 3, 4, 5, 0xffff};
   const uint32_t indices32[] = {0xffffffff, 0, 1, 2, 0xffffffff, 0xffffffff, 3, 4, 5, 0xffffffff};
   uint64_t vertices, primitives;
   rd_restart_counts(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 0,
      (const uint8_t *)indices16, 10, 2, &vertices, &primitives);
   assert(vertices == 6 && primitives == 2);
   rd_restart_counts(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, 0,
      (const uint8_t *)indices32, 10, 4, &vertices, &primitives);
   assert(vertices == 6 && primitives == 2);
   rd_restart_counts(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, 0,
      (const uint8_t *)indices16, 10, 2, &vertices, &primitives);
   assert(vertices == 6 && primitives == 4);
   const uint16_t empty[] = {0xffff, 0xffff};
   rd_restart_counts(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 0,
      (const uint8_t *)empty, 2, 2, &vertices, &primitives);
   assert(!vertices && !primitives);
   assert(rd_primitive_count(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 9, 0) == 3);
}

static void test_vertex_bounds(void)
{
   assert(rd_vertex_fetch_size(256, 32, 8, VK_WHOLE_SIZE) == 216);
   assert(rd_vertex_fetch_size(256, 32, 8, 24) == 16);
   assert(rd_vertex_fetch_size(256, 32, 8, 8) == 0);
   assert(rd_vertex_fetch_size(256, 32, 8, 0) == 0);
   assert(rd_vertex_fetch_size(256, 256, 0, VK_WHOLE_SIZE) == 0);
   assert(rd_vertex_fetch_size(256, UINT64_MAX, 8, 24) == 0);
   assert(rd_vertex_fetch_size(256, 255, UINT32_MAX, 24) == 0);
   assert(rd_vertex_fetch_size(UINT64_MAX, 0, 0, VK_WHOLE_SIZE) == UINT32_MAX);
}

static void test_texture_pitch(void)
{
   assert(rd_texture_array_pitch_fits(0));
   assert(rd_texture_array_pitch_fits(4096));
   assert(rd_texture_array_pitch_fits(67104768));
   assert(!rd_texture_array_pitch_fits(67100673));
   assert(!rd_texture_array_pitch_fits(67108864));
   assert(!rd_texture_array_pitch_fits(UINT64_C(1) << 32));

   struct rd_resource resource = { .type = A5XX_TEX_2D, .depth = 2, .array_pitch = 67108864 };
   assert(!rd_texture_array_pitch_fits(rd_resource_texture_pitch(&resource)));
   resource.depth = 1;
   assert(rd_resource_texture_pitch(&resource) == 0);
   resource.type = A5XX_TEX_CUBE;
   assert(!rd_texture_array_pitch_fits(rd_resource_texture_pitch(&resource)));
   resource.image_size_only = true;
   assert(rd_resource_texture_pitch(&resource) == 0);
}

static void test_multiview(void)
{
   struct rd_device device = {0};
   list_inithead(&device.active_queries);
   struct rd_query_pool pool = {.vk.query_type = VK_QUERY_TYPE_PIPELINE_STATISTICS};
   assert(mtx_init(&pool.mutex, mtx_plain) == thrd_success);
   assert(cnd_init(&pool.condition) == thrd_success);
   for (unsigned i = 0; i < 8; i++) {
      pool.slots[i].pool = &pool;
      list_inithead(&pool.slots[i].active_link);
   }
   struct rd_render render = {0};
   assert(!rd_render_needs_query_accounting(&device, &render));
   struct rd_render_query query = {.pool = &pool};
   for (unsigned i = 0; i < 2; i++) {
      query.query = i; query.zero = i != 0;
      rd_render_query_op(&device, &query);
   }
   assert(pool.slots[0].active && !pool.slots[1].active);
   assert(rd_render_needs_query_accounting(&device, &render));
   struct rd_draw draw = {.vertex_count = 3, .instance_count = 1};
   assert(rd_account_draw(&device, &draw, 4, 4) == VK_SUCCESS);
   assert(rd_account_draw(&device, &draw, 4, 4) == VK_SUCCESS);
   query.end = true;
   for (unsigned i = 0; i < 2; i++) {
      query.query = i; query.zero = i != 0;
      rd_render_query_op(&device, &query);
   }
   assert(pool.slots[0].values[0] == 6 && pool.slots[1].values[0] == 0);
   assert(pool.slots[0].available && pool.slots[1].available);
   assert(list_is_empty(&device.active_queries));
   cnd_destroy(&pool.condition); mtx_destroy(&pool.mutex);
}

static void test_trace(void)
{
   struct rd_trace trace = {0};
   struct fd_bo bo = {.map = calloc(RD_TRACE_SLOTS * 2, sizeof(uint64_t))};
   assert(bo.map);
   trace.file = tmpfile(); assert(trace.file);
   trace.bo = &bo; trace.map = bo.map;
   struct rd_device device = {.trace = &trace};
   struct fd_ringbuffer ring = {0};
   rd_trace_command_begin(&device);
   const uint64_t submits = RD_TRACE_SLOTS * 2 + 3;
   for (uint64_t i = 0; i < submits; i++) {
      next_start = (i + 1) * 100000;
      next_end = next_start + i + 1;
      rd_trace_submit_begin(&device, &ring);
      rd_trace_submit_end(&device, &ring);
      device.submit_count++;
   }
   struct rd_trace_record info = {.seq = 7, .type = 0};
   rd_trace_command_end(&device, &info);
   assert(trace.current.slot_total == submits);
   rd_trace_resolve(&device, 1);
   rewind(trace.file);
   char line[2048];
   bool found = false;
   while (fgets(line, sizeof(line), trace.file)) {
      if (strncmp(line, "C seq=7 ", 8)) continue;
      char expected[96];
      snprintf(expected, sizeof(expected), " gpu=%" PRIu64 " ", submits * (submits + 1) / 2);
      assert(strstr(line, expected));
      snprintf(expected, sizeof(expected), " slots=%" PRIu64 " invalid=0", submits);
      assert(strstr(line, expected));
      found = true;
   }
   assert(found && prep_calls >= 3);
   assert(!trace.pending_slots && !trace.records.size);
   fail_growth = true;
   rd_trace_command_begin(&device);
   rd_trace_command_end(&device, &info);
   fail_growth = false;
   prep_error = -1;
   trace.pending_slots = RD_TRACE_SLOTS;
   rd_trace_command_begin(&device);
   rd_trace_submit_begin(&device, &ring);
   assert(!trace.open && trace.current.invalid == 1);
   prep_error = 0;
   fclose(trace.file); free(trace.records.data); free(bo.map);
}

int main(void)
{
   test_growth(); test_restart(); test_vertex_bounds(); test_texture_pitch(); test_image_pitch();
   test_ring_backings(); test_copy_side();
   test_render_query_before_work(); test_drain_index(); test_submit_diag_commands();
   test_descriptor_layout_bindings(); test_descriptor_span(); test_descriptor_template_data();
   test_ycbcr_formats(); test_descriptor_ycbcr_conversion();
   test_ycbcr_sampler_slots(); test_ycbcr_texture_packets();
   test_clear_ds_recording();
   test_query_range(); test_vertex_input(); test_push_constants();
   test_fence_export(); test_sync_policy(); test_compute_tail_batch(); test_sync_waits();
   test_dispatch_preflight(); test_draw_descriptor_cache_enabled();
   test_draw_descriptor_cache(); test_draw_descriptor_set_generations();
   test_device_extensions();
   test_blit_src();
   test_subword();
   test_multiview(); test_trace();
   struct rd_device device = {0};
   assert(rd_init_border_colors(&device) == VK_ERROR_OUT_OF_HOST_MEMORY);
   assert(!device.border_colors && deleted_bos == 1);
   puts("CORRECTNESS_UNIT PASS: OOM, restart, binding bounds, descriptor layouts, spans, template data and YCbCr validation, draw descriptor cache and set generations, texture/image pitch, device extensions, blit capabilities, depth/stencil clear ranges, subword widening, fence export, sync policy, compute tail batch, dispatch preflight, empty-render queries, waits, submit diagnostics, query range, copy direction, ring backings, vertex input, push constants, multiview, trace wrap, creation errors");
   return 0;
}
