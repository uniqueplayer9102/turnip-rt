/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <vulkan/vulkan_core.h>

#include "drm-uapi/drm.h"
#include "../../util/stack_array.h"
#define HAVE_LIBDRM 1
#include "../../util/u_sync_provider.h"

#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))

enum vk_sync_wait_flags { VK_SYNC_WAIT_COMPLETE = 0, VK_SYNC_WAIT_PENDING = 1, VK_SYNC_WAIT_ANY = 2 };
enum vk_sync_flags { VK_SYNC_IS_TIMELINE = 1 };
struct vk_sync { enum vk_sync_flags flags; bool drm; };
struct vk_drm_syncobj { struct vk_sync base; uint32_t syncobj; };
struct vk_sync_wait { struct vk_sync *sync; };
struct vk_physical_device { int unused; };
struct vk_device { struct vk_physical_device *physical; struct util_sync_provider *sync; bool lost; };
struct rd_physical_device {
   struct vk_physical_device vk;
   VkResult (*sync_wait_many)(struct vk_device *, uint32_t, const struct vk_sync_wait *,
                              enum vk_sync_wait_flags, uint64_t);
};

static struct {
   uint64_t now, kernel_deadline, fallback_deadline, slept;
   unsigned kernel_calls, fallback_calls, wakes, kernel_flags;
   unsigned kernel_count;
   int kernel_error;
   VkResult legacy_result;
   bool kernel_enabled, legacy_yield, publish_after_slice, lose_on_wait;
} mock;
static struct vk_device device;

static struct vk_drm_syncobj *
vk_sync_as_drm_syncobj(struct vk_sync *sync)
{
   return sync->drm ? container_of(sync, struct vk_drm_syncobj, base) : NULL;
}

static bool vk_device_is_lost_no_report(struct vk_device *dev) { return dev->lost; }
static void rd_wake_device_waiters(struct vk_device *dev) { assert(dev->lost); mock.wakes++; }
static void vk_device_set_lost(struct vk_device *dev, const char *msg) { assert(msg); dev->lost = true; }
static uint64_t os_time_get_nano(void) { return mock.now; }
static bool debug_get_option_pending_kernel_wait(void) { return mock.kernel_enabled; }
static bool debug_get_option_pending_blocking_wait(void) { return mock.legacy_yield; }

static int
wait_available(struct util_sync_provider *provider, uint32_t *handles, uint64_t *points,
               unsigned count, int64_t deadline, unsigned flags, uint32_t *first)
{
   assert(provider && !first);
   mock.kernel_calls++;
   mock.kernel_count = count;
   mock.kernel_deadline = deadline;
   mock.kernel_flags = flags;
   for (unsigned i = 0; i < count; i++) {
      assert(handles[i] == 12 + i);
      assert(points[i] == 0);
   }
   if (mock.lose_on_wait)
      device.lost = true;
   int ret = mock.kernel_error;
   if (ret == -ETIME && deadline) {
      mock.now = deadline;
      if (mock.publish_after_slice)
         mock.kernel_error = 0;
   }
   return ret;
}

static VkResult
legacy_wait(struct vk_device *dev, uint32_t count, const struct vk_sync_wait *waits,
            enum vk_sync_wait_flags flags, uint64_t deadline)
{
   assert(dev == &device && count && waits);
   assert(!(flags & ~(VK_SYNC_WAIT_PENDING | VK_SYNC_WAIT_ANY)));
   mock.fallback_calls++;
   mock.fallback_deadline = deadline;
   return mock.legacy_result;
}

static int
mock_nanosleep(const struct timespec *duration, struct timespec *remaining)
{
   assert(!remaining && !duration->tv_sec && duration->tv_nsec > 0);
   mock.now += duration->tv_nsec;
   mock.slept += duration->tv_nsec;
   return 0;
}

#include "sync_waits_under_test.h"
#define nanosleep mock_nanosleep
#include "pending_wait_under_test.h"
#undef nanosleep

static struct util_sync_provider provider = { .timeline_wait = wait_available };
static struct rd_physical_device physical = { .sync_wait_many = legacy_wait };

static void
reset(void)
{
   device = (struct vk_device) { .physical = &physical.vk, .sync = &provider };
   mock = (typeof(mock)) { .now = 1000, .kernel_enabled = true };
   provider.timeline_wait = wait_available;
}

int main(void)
{
   struct vk_drm_syncobj objs[9];
   struct vk_sync_wait waits[9];
   for (unsigned i = 0; i < 9; i++) {
      objs[i] = (struct vk_drm_syncobj) { .base.drm = true, .syncobj = 12 + i };
      waits[i].sync = &objs[i].base;
   }
   unsigned available_flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE |
                              DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;

   reset();
   assert(rd_sync_wait_many(&device, 9, waits, VK_SYNC_WAIT_PENDING, 5000) == VK_SUCCESS);
   assert(mock.kernel_calls == 1 && mock.kernel_count == 9 && mock.kernel_deadline == 5000);
   assert(mock.kernel_flags == (available_flags | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL));
   assert(!mock.fallback_calls && !mock.slept);

   reset();
   mock.kernel_error = -ETIME;
   assert(rd_sync_wait_many(&device, 2, waits, VK_SYNC_WAIT_PENDING | VK_SYNC_WAIT_ANY, 0) == VK_TIMEOUT);
   assert(mock.kernel_flags == available_flags && !mock.kernel_deadline && !mock.fallback_calls);

   reset();
   mock.kernel_error = -ETIME;
   mock.publish_after_slice = true;
   assert(rd_sync_wait_many(&device, 1, waits, VK_SYNC_WAIT_PENDING, UINT64_MAX) == VK_SUCCESS);
   assert(mock.kernel_calls == 2 && mock.kernel_deadline == 200001000 && !mock.slept);

   reset();
   mock.kernel_error = -EINVAL;
   mock.legacy_result = VK_TIMEOUT;
   assert(rd_sync_wait_many(&device, 1, waits, VK_SYNC_WAIT_PENDING, 1251000) == VK_TIMEOUT);
   assert(mock.kernel_calls == 1 && mock.fallback_calls == 3 && !mock.fallback_deadline);
   assert(mock.slept == 1250000);

   reset();
   provider.timeline_wait = NULL;
   mock.legacy_yield = true;
   assert(rd_sync_wait_many(&device, 1, waits, VK_SYNC_WAIT_PENDING, 5000) == VK_SUCCESS);
   assert(!mock.kernel_calls && mock.fallback_calls == 1 && mock.fallback_deadline == 5000 && !mock.slept);

   reset();
   mock.kernel_enabled = false;
   assert(rd_sync_wait_many(&device, 1, waits, VK_SYNC_WAIT_PENDING, 5000) == VK_SUCCESS);
   assert(!mock.kernel_calls && mock.fallback_calls == 1 && !mock.fallback_deadline);

   reset();
   assert(rd_sync_wait_many(&device, 1, waits, VK_SYNC_WAIT_COMPLETE, 5000) == VK_SUCCESS);
   assert(!mock.kernel_calls && mock.fallback_calls == 1 && mock.fallback_deadline == 5000);
   objs[1].base.flags = VK_SYNC_IS_TIMELINE;
   assert(rd_sync_wait_many(&device, 2, waits, VK_SYNC_WAIT_PENDING, 5000) == VK_SUCCESS);
   assert(!mock.kernel_calls && mock.fallback_calls == 2 && mock.fallback_deadline == 5000);
   objs[1].base.flags = 0;

   reset();
   mock.kernel_error = -EIO;
   assert(rd_sync_wait_many(&device, 1, waits, VK_SYNC_WAIT_PENDING, 5000) == VK_ERROR_UNKNOWN);
   assert(mock.kernel_calls == 1 && !mock.fallback_calls);

   reset();
   mock.lose_on_wait = true;
   assert(rd_sync_wait_many(&device, 1, waits, VK_SYNC_WAIT_PENDING, 5000) == VK_ERROR_DEVICE_LOST);
   assert(mock.wakes == 1 && !mock.fallback_calls);
   assert(rd_sync_wait_many(&device, 1, waits, VK_SYNC_WAIT_PENDING, 5000) == VK_ERROR_DEVICE_LOST);
   assert(mock.kernel_calls == 1 && mock.wakes == 2);

   puts("PENDING_WAIT PASS: binary fence publication, wait-any/all, timeouts, fallback and device loss");
   return 0;
}
