/* SPDX-License-Identifier: MIT */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

enum { WIDTH = 264, HEIGHT = 192, BORDER = 4 };

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_surface *surface;
static struct wl_buffer *buffer;
static bool mapped, closed;

static void
global(void *data, struct wl_registry *registry, uint32_t name,
       const char *interface, uint32_t version)
{
   (void)data;
   (void)version;
   if (!strcmp(interface, wl_compositor_interface.name))
      compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
   else if (!strcmp(interface, wl_shm_interface.name))
      shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
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

static void
ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
   (void)data;
   xdg_wm_base_pong(base, serial);
}

static void
frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
   (void)data;
   (void)time;
   wl_callback_destroy(callback);
   puts("READY");
   fflush(stdout);
}

static void
configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
   (void)data;
   static const struct wl_callback_listener listener = { .done = frame_done };
   xdg_surface_ack_configure(xdg_surface, serial);
   if (mapped)
      return;
   mapped = true;
   wl_surface_attach(surface, buffer, 0, 0);
   wl_surface_damage(surface, 0, 0, WIDTH, HEIGHT);
   wl_callback_add_listener(wl_surface_frame(surface), &listener, NULL);
   wl_surface_commit(surface);
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
   closed = true;
}

static uint32_t
pattern(unsigned x, unsigned y)
{
   if (x < BORDER || x >= WIDTH - BORDER || y < BORDER || y >= HEIGHT - BORDER)
      return 0xffff00ff;
   uint32_t v = x - BORDER;
   if (y < 64)
      return 0xff000000 | v * 0x010101;
   if (y < 128)
      return 0x80000000 | ((v * 128 + 127) / 255) * 0x010101;
   return 0x80000000 | (v < 85 ? 0x800000 : v < 170 ? 0x008000 : 0x000080);
}

int
main(void)
{
   static const struct wl_registry_listener registry_listener = { global, global_remove };
   static const struct xdg_wm_base_listener wm_listener = { .ping = ping };
   static const struct xdg_surface_listener surface_listener = { .configure = configure };
   static const struct xdg_toplevel_listener toplevel_listener = {
      .configure = toplevel_configure, .close = toplevel_close,
   };

   struct wl_display *display = wl_display_connect(NULL);
   if (!display)
      return 1;
   wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
   if (wl_display_roundtrip(display) < 0 || !compositor || !shm || !wm)
      return 1;
   xdg_wm_base_add_listener(wm, &wm_listener, NULL);

   const size_t bytes = WIDTH * HEIGHT * sizeof(uint32_t);
   int fd = memfd_create("phoc-sdr-color-probe", MFD_CLOEXEC);
   if (fd < 0 || ftruncate(fd, bytes))
      return 1;
   uint32_t *pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (pixels == MAP_FAILED)
      return 1;
   for (unsigned y = 0; y < HEIGHT; y++)
      for (unsigned x = 0; x < WIDTH; x++)
         pixels[y * WIDTH + x] = pattern(x, y);
   munmap(pixels, bytes);

   struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, bytes);
   buffer = wl_shm_pool_create_buffer(pool, 0, WIDTH, HEIGHT, WIDTH * 4, WL_SHM_FORMAT_ARGB8888);
   wl_shm_pool_destroy(pool);
   close(fd);

   surface = wl_compositor_create_surface(compositor);
   struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(wm, surface);
   xdg_surface_add_listener(xdg_surface, &surface_listener, NULL);
   struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
   xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
   xdg_toplevel_set_title(toplevel, "SDR color probe");
   xdg_toplevel_set_app_id(toplevel, "radish-sdr-color-probe");
   wl_surface_commit(surface);
   while (!closed && wl_display_dispatch(display) >= 0) {
   }
   wl_display_disconnect(display);
   return mapped ? 0 : 1;
}
