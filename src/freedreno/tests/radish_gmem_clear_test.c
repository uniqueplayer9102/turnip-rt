#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VK_CULL_MODE_NONE 0
struct rd_pipeline { int dummy; };
struct rd_device { struct rd_pipeline *clear_pipeline; };
struct rd_draw {
   struct rd_pipeline *pipeline;
   uint32_t vertex_count, instance_count, first_vertex, first_instance, sample_mask, cull;
   bool indexed, indirect, rasterizer_discard, alpha_to_coverage;
   struct { struct { bool test_enable; } depth, stencil; } ds;
   struct { struct { unsigned write_mask; bool blend_enable; } attachments[1]; } cb;
   uint32_t push[9];
   struct { float x, y, width, height, minDepth, maxDepth; } viewport;
   struct { struct { int32_t x, y; } offset;
            struct { uint32_t width, height; } extent; } scissor;
};
struct util_dynarray { struct rd_draw *data; unsigned count; };
struct rd_render { struct util_dynarray draws; };
#define util_dynarray_foreach(array, type, item) \
   for (type *item = (array)->data; item < (array)->data + (array)->count; item++)
#include "gmem_clear_under_test.h"

int main(void)
{
   struct rd_pipeline pipeline, other;
   struct rd_device device = { &pipeline };
   struct rd_draw base = { .pipeline = &pipeline, .vertex_count = 4,
      .instance_count = 1, .sample_mask = 0xffff,
      .viewport = {64,96,704,800,0,1}, .scissor = {{64,96},{704,800}} };
   base.cb.attachments[0].write_mask = 15;
   const float rect[4] = {-1,-1,1,1};
   memcpy(&base.push[4], rect, sizeof(rect));
   struct rd_draw draws[] = {{0},base};
   struct rd_render render = {{draws,2}};
   assert(rd_gmem_first_clear(&device, &render) == &draws[1]);
   assert(rd_gmem_clear_covers_tile(&base,64,256,64,256));
   assert(!rd_gmem_clear_covers_tile(&base,0,256,64,256));
   assert(!rd_gmem_clear_covers_tile(&base,64,0,64,256));
   assert(!rd_gmem_clear_covers_tile(&base,704,768,128,256));
   assert(!rd_gmem_clear_covers_tile(NULL,64,256,64,256));
   for (unsigned test = 0; test < 14; test++) {
      draws[1] = base;
      switch (test) {
      case 0: draws[1].pipeline = &other; break;
      case 1: draws[1].indexed = true; break;
      case 2: draws[1].indirect = true; break;
      case 3: draws[1].vertex_count = 3; break;
      case 4: draws[1].instance_count = 2; break;
      case 5: draws[1].first_vertex = 1; break;
      case 6: draws[1].first_instance = 1; break;
      case 7: draws[1].rasterizer_discard = true; break;
      case 8: draws[1].ds.depth.test_enable = true; break;
      case 9: draws[1].ds.stencil.test_enable = true; break;
      case 10: draws[1].cb.attachments[0].write_mask = 7; break;
      case 11: draws[1].cb.attachments[0].blend_enable = true; break;
      case 12: draws[1].sample_mask = 2; break;
      case 13: draws[1].push[4] = 0; break;
      }
      assert(!rd_gmem_first_clear(&device, &render));
   }
   draws[0] = base;
   draws[0].pipeline = &other;
   draws[1] = base;
   assert(!rd_gmem_first_clear(&device, &render));
   base.scissor.offset.x = -100;
   base.scissor.extent.width = 1;
   assert(!rd_gmem_clear_covers_tile(&base,64,256,64,256));
   base.scissor.offset.x = 64;
   base.scissor.extent.width = 704;
   base.viewport.height = -800;
   assert(!rd_gmem_clear_covers_tile(&base,64,256,64,256));
   puts("GMEM_CLEAR PASS: known clears, coverage, zero draws, prior draws, clipping and state exclusions");
   return 0;
}
