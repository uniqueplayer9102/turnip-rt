#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VKAPI_ATTR
#define VKAPI_CALL
#define VK_ERROR_UNKNOWN 1
#define VK_FROM_HANDLE(type, name, handle) struct type *name = handle
typedef struct rd_command_buffer *VkCommandBuffer;
typedef struct { uint32_t firstVertex, vertexCount; } VkMultiDrawInfoEXT;
typedef struct { uint32_t firstIndex, indexCount; int32_t vertexOffset; } VkMultiDrawIndexedInfoEXT;
#define vk_foreach_multi_draw(info, i, data, count, stride) \
   for (const VkMultiDrawInfoEXT *info = data; i < count; \
        i++, info = (const void *)((const uint8_t *)info + stride))
#define vk_foreach_multi_draw_indexed(info, i, data, count, stride) \
   for (const VkMultiDrawIndexedInfoEXT *info = data; i < count; \
        i++, info = (const void *)((const uint8_t *)info + stride))
struct rd_draw {
   bool barrier, indexed;
   uint32_t vertex_count, instance_count, first_vertex, first_instance, first_index;
   int32_t vertex_offset;
   const void *descriptors;
};
struct rd_render { bool descriptor_arena; };
struct rd_command_buffer {
   struct rd_render storage, *render;
   bool index_buffer;
   int vk;
   unsigned count, snapshots, fail_after;
   struct rd_draw draws[32];
};
static void rd_set_error(int *vk, int error) { *vk = error; }
static struct rd_draw *
rd_render_append_draw(struct rd_command_buffer *command, struct rd_render *render,
                      const struct rd_draw *state)
{
   assert(render == command->render);
   if (command->count == command->fail_after) {
      command->vk = 2;
      return NULL;
   }
   assert(command->count < 32);
   struct rd_draw *draw = &command->draws[command->count++];
   *draw = *state;
   return draw;
}
static struct rd_draw *rd_record_draw(struct rd_command_buffer *command)
{
   command->snapshots++;
   struct rd_draw state = { .barrier = command->count == 0,
                           .descriptors = &command->snapshots };
   return rd_render_append_draw(command, command->render, &state);
}
#include "multi_draw_under_test.h"
static struct rd_command_buffer command;
static void reset(bool arena)
{
   memset(&command, 0, sizeof(command));
   command.storage.descriptor_arena = arena;
   command.render = &command.storage;
   command.index_buffer = true;
   command.fail_after = 32;
}
int main(void)
{
   struct { VkMultiDrawInfoEXT info; uint32_t padding[2]; } vertices[] = {
      { { 9, 0 }, { 0, 0 } }, { { 5, 3 }, { 0, 0 } }, { { 12, 6 }, { 0, 0 } }
   };
   for (unsigned arena = 0; arena < 2; arena++) {
      reset(arena);
      rd_CmdDrawMultiEXT(&command, 3, &vertices[0].info, 2, 7, sizeof(vertices[0]));
      assert(command.count == 2 && command.snapshots == (arena ? 1 : 2));
      assert(command.draws[0].first_vertex == 5 && command.draws[1].first_vertex == 12);
      assert(command.draws[0].barrier && !command.draws[1].barrier);
      assert(command.draws[1].instance_count == 2 && command.draws[1].first_instance == 7);
      reset(arena);
      VkMultiDrawInfoEXT repeated = { 8, 3 };
      rd_CmdDrawMultiEXT(&command, 1, &repeated, 1, 0, 0);
      assert(command.count == 1 && command.draws[0].first_vertex == 8);
      reset(arena);
      VkMultiDrawIndexedInfoEXT indices[] = { { 0, 0, 90 }, { 4, 3, -2 }, { 9, 6, 5 } };
      rd_CmdDrawMultiIndexedEXT(&command, 3, indices, 3, 11, sizeof(indices[0]), NULL);
      assert(command.count == 2 && command.snapshots == (arena ? 1 : 2));
      assert(command.draws[0].indexed && command.draws[1].indexed);
      assert(command.draws[0].first_index == 4 && command.draws[0].vertex_offset == -2);
      assert(command.draws[1].first_index == 9 && command.draws[1].vertex_offset == 5);
      assert(command.draws[1].instance_count == 3 && command.draws[1].first_instance == 11);
      reset(arena);
      uint32_t compact[] = { 0, 3, 123, 6, 3, 456 };
      int32_t offset = -4;
      rd_CmdDrawMultiIndexedEXT(&command, 2, (const void *)compact, 1, 0, 12, &offset);
      assert(command.count == 2 && command.draws[1].vertex_offset == -4);
      assert(command.draws[1].first_index == 6);
      reset(arena);
      rd_CmdDrawMultiEXT(&command, 0, NULL, 1, 0, 0);
      rd_CmdDrawMultiIndexedEXT(&command, 2, NULL, 0, 0, 0, NULL);
      assert(!command.count && !command.snapshots && !command.vk);
      command.index_buffer = false;
      rd_CmdDrawMultiIndexedEXT(&command, 1, indices, 1, 0, sizeof(indices[0]), NULL);
      assert(command.vk == VK_ERROR_UNKNOWN && !command.count);
      reset(arena);
      command.fail_after = 1;
      VkMultiDrawInfoEXT several[] = {repeated, repeated, repeated, repeated};
      rd_CmdDrawMultiEXT(&command, 4, several, 1, 0, sizeof(several[0]));
      assert(command.count == 1 && command.vk == 2);
   }
   puts("MULTI_DRAW PASS: strides, zero draws, instances, signed offsets, shared state, arena fallback, OOM");
   return 0;
}
