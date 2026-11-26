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
struct fd_bo { uint64_t *map; };
struct fd_ringbuffer { unsigned unused; };
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
   struct { VkQueryType query_type; } vk;
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
struct rd_render { struct util_dynarray draws, queries; };
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
   struct { uint32_t array_layers; VkImageUsageFlags usage; } vk;
   struct rd_image_level levels[1];
};
#include "image_pitch_under_test.h"

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
   test_device_extensions();
   test_blit_src();
   test_subword();
   test_multiview(); test_trace();
   struct rd_device device = {0};
   assert(rd_init_border_colors(&device) == VK_ERROR_OUT_OF_HOST_MEMORY);
   assert(!device.border_colors && deleted_bos == 1);
   puts("CORRECTNESS_UNIT PASS: OOM, restart, binding bounds, texture/image pitch, device extensions, blit capabilities, subword widening, multiview, trace wrap, creation errors");
   return 0;
}
