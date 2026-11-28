/* SPDX-License-Identifier: MIT */

#define _GNU_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define FRAMES 600

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wp_presentation *presentation;
static struct xdg_wm_base *wm;
static struct wl_surface *surface;
static struct wl_buffer *buffer;
static int frame_pending;
static int submitted, presented, feedback_pending, first;
static uint64_t last_ns, refresh_ns;
static uint64_t intervals[FRAMES];
static int interval_count, missed;

static void
xdg_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
   (void)data;
   xdg_wm_base_pong(base, serial);
}

static void
xdg_configure(void *data, struct xdg_surface *xsurface, uint32_t serial)
{
   (void)data;
   xdg_surface_ack_configure(xsurface, serial);
}

static void
toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width,
                   int32_t height, struct wl_array *states)
{
   (void)data;
   (void)toplevel;
   (void)width;
   (void)height;
   (void)states;
}

static void
toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
   (void)data;
   (void)toplevel;
   first = 0;
}

static void
frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
   (void)data;
   (void)time;
   wl_callback_destroy(callback);
   frame_pending = 0;
}

static void
feedback_sync(void *data, struct wp_presentation_feedback *feedback,
              struct wl_output *output)
{
   (void)data;
   (void)feedback;
   (void)output;
}

static void
feedback_presented(void *data, struct wp_presentation_feedback *feedback,
                   uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec,
                   uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo,
                   uint32_t flags)
{
   (void)data;
   (void)seq_hi;
   (void)seq_lo;
   (void)flags;
   uint64_t now = ((uint64_t)sec_hi << 32 | sec_lo) * 1000000000ull + nsec;
   refresh_ns = refresh;
   if (last_ns && interval_count < FRAMES) {
      uint64_t delta = now - last_ns;
      intervals[interval_count++] = delta;
      if (refresh && delta > refresh + refresh / 2) {
         missed++;
         printf("MISS at_ns=%" PRIu64 " delta_ns=%" PRIu64 "\n", now, delta);
      }
   }
   last_ns = now;
   presented++;
   feedback_pending--;
   wp_presentation_feedback_destroy(feedback);
}

static void
feedback_discarded(void *data, struct wp_presentation_feedback *feedback)
{
   (void)data;
   wp_presentation_feedback_destroy(feedback);
   feedback_pending--;
}

static void
global(void *data, struct wl_registry *registry, uint32_t name,
       const char *interface, uint32_t version)
{
   (void)data;
   (void)version;
   if (!strcmp(interface, wl_compositor_interface.name))
      compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 2);
   else if (!strcmp(interface, wl_shm_interface.name))
      shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
   else if (!strcmp(interface, wp_presentation_interface.name))
      presentation = wl_registry_bind(registry, name, &wp_presentation_interface, 1);
   else if (!strcmp(interface, xdg_wm_base_interface.name))
      wm = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
}

static void
global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
   (void)data;
   (void)registry;
   (void)name;
}

static struct wl_buffer *
make_buffer(void)
{
   const size_t bytes = 256 * 256 * 4;
   int fd = memfd_create("present-stream", MFD_CLOEXEC);
   if (fd < 0 || ftruncate(fd, bytes))
      exit(1);
   struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, bytes);
   struct wl_buffer *out = wl_shm_pool_create_buffer(pool, 0, 256, 256,
                                                     256 * 4, WL_SHM_FORMAT_ARGB8888);
   wl_shm_pool_destroy(pool);
   close(fd);
   return out;
}

static int
cmp_u64(const void *a, const void *b)
{
   uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
   return x < y ? -1 : x > y ? 1 : 0;
}

int
main(void)
{
   static const struct wl_registry_listener registry_listener = { global, global_remove };
   static const struct xdg_wm_base_listener wm_listener = { .ping = xdg_ping };
   static const struct xdg_surface_listener surface_listener = { .configure = xdg_configure };
   static const struct xdg_toplevel_listener toplevel_listener = {
      .configure = toplevel_configure, .close = toplevel_close,
   };

   display = wl_display_connect(NULL);
   if (!display)
      return 1;
   wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
   if (wl_display_roundtrip(display) < 0 || !compositor || !shm || !presentation || !wm)
      return 1;
   xdg_wm_base_add_listener(wm, &wm_listener, NULL);
   surface = wl_compositor_create_surface(compositor);
   buffer = make_buffer();
   struct xdg_surface *xsurface = xdg_wm_base_get_xdg_surface(wm, surface);
   xdg_surface_add_listener(xsurface, &surface_listener, NULL);
   struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xsurface);
   xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
   xdg_toplevel_set_title(toplevel, "present-stream");
   xdg_toplevel_set_app_id(toplevel, "radish-present-stream");
   wl_surface_commit(surface);
   if (wl_display_roundtrip(display) < 0)
      return 1;
   first = 1;

   while (first && submitted < FRAMES && wl_display_dispatch(display) >= 0) {
      if (frame_pending)
         continue;
      wl_surface_attach(surface, buffer, 0, 0);
      wl_surface_damage(surface, 0, 0, 256, 256);
      static const struct wl_callback_listener frame_listener = { .done = frame_done };
      wl_callback_add_listener(wl_surface_frame(surface), &frame_listener, NULL);
      frame_pending = 1;
      static const struct wp_presentation_feedback_listener feedback_listener = {
         .sync_output = feedback_sync,
         .presented = feedback_presented,
         .discarded = feedback_discarded,
      };
      struct wp_presentation_feedback *feedback = wp_presentation_feedback(presentation, surface);
      wp_presentation_feedback_add_listener(feedback, &feedback_listener, NULL);
      feedback_pending++;
      wl_surface_commit(surface);
      submitted++;
   }

   while (feedback_pending && wl_display_dispatch(display) >= 0) {
   }
   wl_display_disconnect(display);

   if (!interval_count) {
      puts("no presentation feedback received");
      return 0;
   }
   qsort(intervals, interval_count, sizeof(intervals[0]), cmp_u64);
   uint64_t sum = 0;
   for (int i = 0; i < interval_count; i++)
      sum += intervals[i];
   printf("submitted=%d presented=%d refresh_ns=%" PRIu64 "\n",
          submitted, presented, refresh_ns);
   printf("mean_ns=%" PRIu64 " p50_ns=%" PRIu64 " p95_ns=%" PRIu64
          " max_ns=%" PRIu64 " missed=%d\n",
          sum / interval_count, intervals[interval_count / 2],
          intervals[(interval_count * 95) / 100], intervals[interval_count - 1], missed);
   return 0;
}
