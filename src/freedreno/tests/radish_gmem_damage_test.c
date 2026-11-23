#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct rd_gmem_draw_seg { int64_t minx, miny, maxx, maxy; };
#include "gmem_damage_under_test.h"

int main(void)
{
   struct rd_gmem_draw_seg draws[] = {{64,96,768,896},{0,0,0,0},{900,1024,1003,1997}};
   assert(!rd_gmem_tile_has_draws(draws,0,64,256,64,256));
   assert(rd_gmem_tile_has_draws(draws,3,64,256,64,256));
   assert(!rd_gmem_tile_has_draws(draws,3,0,256,64,256));
   assert(!rd_gmem_tile_has_draws(draws,3,768,256,128,256));
   assert(rd_gmem_tile_has_draws(draws,3,896,1024,128,256));
   assert(!rd_gmem_tile_has_draws(draws,3,896,768,128,256));
   draws[0] = (struct rd_gmem_draw_seg){-100,-100,-1,-1};
   assert(!rd_gmem_tile_has_draws(draws,1,0,0,64,256));
   draws[0] = (struct rd_gmem_draw_seg){63,255,65,257};
   assert(rd_gmem_tile_has_draws(draws,1,0,0,64,256));
   assert(rd_gmem_tile_has_draws(draws,1,64,256,64,256));
   draws[0] = (struct rd_gmem_draw_seg){0,0,8192,8192};
   assert(rd_gmem_tile_has_draws(draws,1,8128,7936,64,256));
   draws[0] = (struct rd_gmem_draw_seg){100,100,20,20};
   assert(!rd_gmem_tile_has_draws(draws,1,0,0,128,128));
   puts("GMEM_DAMAGE PASS: half-open bounds, partial tiles, empty and negative regions, multiple draws");
   return 0;
}
