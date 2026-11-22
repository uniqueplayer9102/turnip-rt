#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum fd_pipe_id { FD_PIPE_3D, FD_PIPE_2D, FD_PIPE_MAX };
enum { FD_VERSION_SUBMIT_QUEUES = 3, FD_GPU_ID, FD_CHIP_ID,
       FD_BO_CACHED_COHERENT = 1, _FD_BO_NOSYNC = 2, NO_CACHE = 0 };
struct fd_dev_id { uint32_t gpu_id; uint64_t chip_id; };
struct control { uint32_t fence; };
struct fd_bo { struct control control; int bo_reuse; };
struct fd_pipe;
struct fd_device;
struct pipe_funcs { void (*destroy)(struct fd_pipe *); };
struct device_funcs { struct fd_pipe *(*pipe_new)(struct fd_device *, enum fd_pipe_id, uint32_t); };
struct fd_device { const struct device_funcs *funcs; };
struct fd_pipe {
   struct fd_device *dev;
   enum fd_pipe_id id;
   int refcnt, gen, is_64bit;
   struct fd_dev_id dev_id;
   struct fd_bo *control_mem;
   struct control *control;
   const struct pipe_funcs *funcs;
};
static int scenario, destroyed, released;
#define ERROR_MSG(...) ((void)0)
#define p_atomic_set(pointer, value) (*(pointer) = (value))
static int fd_device_version(struct fd_device *device) { (void)device; return 10; }
static int fd_pipe_get_param(struct fd_pipe *pipe, int param, uint64_t *value)
{
   (void)pipe;
   if ((scenario == 4 && param == FD_CHIP_ID) || (scenario == 5 && param == FD_GPU_ID))
      return -1;
   *value = param == FD_GPU_ID ? 506 : UINT64_C(0x05000600);
   return 0;
}
static int fd_dev_is_supported(const struct fd_dev_id *id)
{
   return scenario != 3 && id->gpu_id == 506 &&
      (id->chip_id == UINT64_C(0x05000600) || id->chip_id == 0);
}
static int fd_dev_gen(const struct fd_dev_id *id) { (void)id; return 5; }
static int fd_dev_64b(const struct fd_dev_id *id) { (void)id; return 1; }
static struct fd_bo *fd_bo_new(struct fd_device *device, unsigned size, int flags, const char *name)
{
   (void)device; (void)size; (void)flags; (void)name;
   return scenario == 1 ? NULL : calloc(1, sizeof(struct fd_bo));
}
static void *fd_bo_map(struct fd_bo *bo) { return scenario == 2 ? NULL : &bo->control; }
static void fd_bo_del(struct fd_bo *bo) { assert(bo); released++; free(bo); }
static void destroy(struct fd_pipe *pipe) { destroyed++; free(pipe); }
static const struct pipe_funcs operations = { .destroy = destroy };
static struct fd_pipe *pipe_new(struct fd_device *device, enum fd_pipe_id id, uint32_t priority)
{
   (void)device; (void)id; (void)priority;
   struct fd_pipe *pipe = calloc(1, sizeof(*pipe));
   assert(pipe);
   pipe->funcs = &operations;
   return pipe;
}

#include "pipe_under_test.h"

int main(int argc, char **argv)
{
   assert(argc == 2);
   scenario = atoi(argv[1]);
   const struct device_funcs functions = { .pipe_new = pipe_new };
   struct fd_device device = { .funcs = &functions };
   struct fd_pipe *pipe = fd_pipe_new2(&device, FD_PIPE_3D, 1);
   if (scenario == 0 || scenario == 4) {
      assert(pipe && pipe->control && pipe->control->fence == 0);
      if (scenario == 4)
         assert(pipe->dev_id.chip_id == 0);
      fd_bo_del(pipe->control_mem);
      pipe->funcs->destroy(pipe);
   } else {
      assert(!pipe);
   }
   assert(destroyed == 1);
   assert(released == (scenario == 0 || scenario == 2 || scenario == 4));
   printf("PIPE_CASE %d PASS destroyed=%d released=%d\n", scenario, destroyed, released);
   return 0;
}
