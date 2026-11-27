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

#define MIN2(a, b) ((a) < (b) ? (a) : (b))
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
};
struct rd_device {
   struct rd_trace *trace;
   uint64_t submit_count;
   struct list_head active_queries;
   void *pipe, *fd;
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
   enum a5xx_tex_type type;
   uint32_t depth, array_pitch;
   bool image_size_only;
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
struct rd_image {
   struct { uint32_t array_layers; VkImageUsageFlags usage; VkFormat format;
            VkImageType image_type; VkSampleCountFlagBits samples; } vk;
   struct { uint32_t tile_mode; } native_layout;
   struct rd_image_level levels[1];
   struct rd_memory *memory;
   VkDeviceSize memory_offset, size;
};
#include "image_pitch_under_test.h"

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

struct rd_binding { uint32_t slot, count; VkDescriptorType type; };
struct rd_set_layout {
   uint32_t descriptor_count, binding_count;
   struct rd_binding bindings[8];
};
struct rd_memory { struct { VkDeviceSize size; } vk; struct fd_bo *bo; };
struct rd_buffer { struct { VkDeviceSize size; } vk; struct rd_memory *memory; VkDeviceSize offset; };
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

#include "descriptor_span_under_test.h"

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

static void test_descriptor_span(void)
{
   struct rd_set_layout layout = {
      .descriptor_count = 6,
      .binding_count = 3,
      .bindings = {
         {0, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
         {2, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
         {4, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
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
   test_descriptor_span();
   test_query_range(); test_vertex_input();
   test_fence_export(); test_sync_policy(); test_compute_tail_batch(); test_sync_waits();
   test_device_extensions();
   test_blit_src();
   test_subword();
   test_multiview(); test_trace();
   struct rd_device device = {0};
   assert(rd_init_border_colors(&device) == VK_ERROR_OUT_OF_HOST_MEMORY);
   assert(!device.border_colors && deleted_bos == 1);
   puts("CORRECTNESS_UNIT PASS: OOM, restart, binding bounds, descriptor spans, texture/image pitch, device extensions, blit capabilities, subword widening, fence export, sync policy, compute tail batch, empty-render queries, waits, submit diagnostics, query range, copy direction, ring backings, vertex input, multiview, trace wrap, creation errors");
   return 0;
}
