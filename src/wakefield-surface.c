/*
 * Copyright (C) 2015 Endless OS Foundation LLC
 * Copyright (C) 2023 Canonical Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 * Written by:
 *     Jasper St. Pierre <jstpierre@mecheye.net>
 *     Alexander Larsson <alexl@redhat.com>
 *     Marco Trevisan <marco.trevisan@canonical.com>
 */

#include "config.h"

#include <string.h>

#include "wakefield-private.h"
#include "xdg-shell-server-protocol.h"

#define WAKEFIELD_TYPE_SURFACE (wakefield_surface_get_type ())

G_DECLARE_FINAL_TYPE (WakefieldSurface, wakefield_surface, WAKEFIELD, SURFACE, GObject);

typedef struct _WakefieldXdgPopup WakefieldXdgPopup;

static void xdg_surface_get_toplevel (struct wl_client *client,
                                      struct wl_resource *resource,
                                      uint32_t id);

static void xdg_surface_get_popup (struct wl_client *client,
                                   struct wl_resource *resource,
                                   uint32_t id,
                                   struct wl_resource *parent,
                                   struct wl_resource *positioner);

static void xdg_popup_compute_allocation (WakefieldXdgPopup *xdg_popup,
                                          gboolean           use_surface_size);

enum {
  COMMITTED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

typedef struct _WakefieldSurfacePendingState
{
  struct wl_resource *buffer;
  int scale;

  cairo_region_t *input_region;
  struct wl_list frame_callbacks;
} WakefieldSurfacePendingState;

typedef struct _WakefieldXdgSurface
{
  WakefieldSurface *surface;

  struct wl_resource *resource;
  GdkWindow *window;
} WakefieldXdgSurface;

typedef struct _WakefieldXdgToplevel
{
  WakefieldSurface *surface;

  struct wl_resource *resource;
  GdkWindow *window;
} WakefieldXdgToplevel;

typedef struct _WakeFieldXdgPositioner
{
  cairo_rectangle_int_t anchor_rect;
  int32_t width;
  int32_t height;
  enum xdg_positioner_gravity gravity;
  enum xdg_positioner_anchor anchor;
  enum xdg_positioner_constraint_adjustment constraint_adjustment;
  int32_t offset_x;
  int32_t offset_y;

  gboolean is_reactive;

  gboolean has_parent_size;
  int32_t parent_width;
  int32_t parent_height;

  gboolean acked_parent_configure;
  uint32_t parent_configure_serial;
} WakeFieldXdgPositioner;

typedef struct _WakefieldXdgPopup
{
  WakefieldSurface *surface;
  WakefieldSurface *parent_surface;
  WakeFieldXdgPositioner xdg_positioner;
  cairo_rectangle_int_t allocation;
  uint32_t grab_serial;

  struct wl_resource *resource;
} WakefieldXdgPopup;

struct _WakefieldSurface
{
  GObject parent;

  WakefieldCompositor *compositor;
  struct wl_resource *resource;

  WakefieldSurfaceRole role;

  WakefieldXdgSurface *xdg_surface;
  WakefieldXdgToplevel *xdg_toplevel;
  WakefieldXdgPopup *xdg_popup;

  cairo_region_t *damage;
  WakefieldSurfacePendingState pending, current;
  gboolean mapped;
};

G_DEFINE_FINAL_TYPE (WakefieldSurface, wakefield_surface, G_TYPE_OBJECT);

typedef struct wl_shm_buffer WlShmBufferLocker;
static WlShmBufferLocker *
wl_shm_buffer_locker (struct wl_shm_buffer *shm_buffer)
{
  if (shm_buffer)
    wl_shm_buffer_begin_access (shm_buffer);

  return shm_buffer;
}

static void
wl_shm_buffer_unlocker (WlShmBufferLocker *shm_buffer)
{
  wl_shm_buffer_end_access (shm_buffer);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (WlShmBufferLocker, wl_shm_buffer_unlocker);

struct wl_resource *
wakefield_surface_get_xdg_surface  (struct wl_resource  *surface_resource)
{
  WakefieldSurface *surface = wl_resource_get_user_data (surface_resource);

  if (surface->xdg_surface)
    return surface->xdg_surface->resource;
  return NULL;
}

struct wl_resource *
wakefield_xdg_surface_get_xdg_toplevel (struct wl_resource *xdg_surface_resource)
{
  WakefieldSurface *surface =
    wakefield_xdg_surface_get_surface (xdg_surface_resource);

  if (surface && surface->xdg_toplevel)
    return surface->xdg_toplevel->resource;

  return NULL;
}

struct wl_resource *
wakefield_xdg_surface_get_xdg_popup  (struct wl_resource *xdg_surface_resource)
{
  WakefieldSurface *surface =
    wakefield_xdg_surface_get_surface (xdg_surface_resource);

  if (surface && surface->xdg_popup)
    return surface->xdg_popup->resource;

  return NULL;
}

WakefieldSurfaceRole
wakefield_surface_get_role (struct wl_resource  *surface_resource)
{
  WakefieldSurface *surface = wl_resource_get_user_data (surface_resource);

  return surface->role;
}

void
wakefield_surface_set_role (struct wl_resource *surface_resource,
                            WakefieldSurfaceRole role)
{
  WakefieldSurface *surface = wl_resource_get_user_data (surface_resource);

  g_assert (surface->role == WAKEFIELD_SURFACE_ROLE_NONE ||
            surface->role == role);

  surface->role = role;
}

gboolean
wakefield_surface_is_mapped (struct wl_resource  *surface_resource)
{
  WakefieldSurface *surface = wl_resource_get_user_data (surface_resource);

  return surface->mapped;
}

GdkWindow *
wakefield_surface_get_window (struct wl_resource  *surface_resource)
{
  WakefieldSurface *surface = wl_resource_get_user_data (surface_resource);

  if (surface->xdg_surface)
    return wakefield_xdg_surface_get_window (surface->xdg_surface->resource);

  return NULL;
}

static void
wakefield_surface_get_current_size (WakefieldSurface *surface,
                                    int *width, int *height)
{
  struct wl_shm_buffer *shm_buffer;

  *width = 0;
  *height = 0;

  if (!surface->current.buffer)
    return;

  shm_buffer = wl_shm_buffer_get (surface->current.buffer);
  if (shm_buffer)
    {
      g_autoptr (WlShmBufferLocker) locked = wl_shm_buffer_locker (shm_buffer);
      *width = wl_shm_buffer_get_width (shm_buffer) / surface->current.scale;
      *height = wl_shm_buffer_get_height (shm_buffer) / surface->current.scale;
    }
}

static cairo_format_t
cairo_format_for_wl_shm_format (enum wl_shm_format format)
{
  switch (format)
    {
    case WL_SHM_FORMAT_ARGB8888:
      return CAIRO_FORMAT_ARGB32;
    case WL_SHM_FORMAT_XRGB8888:
      return CAIRO_FORMAT_RGB24;
    default:
      g_assert_not_reached ();
    }
}

WakefieldCompositor *
wakefield_surface_get_compositor (WakefieldSurface *surface)
{
  return surface->compositor;
}

cairo_surface_t *
wakefield_surface_create_cairo_surface (WakefieldSurface *surface,
                                        int *width_out, int *height_out)
{
  struct wl_shm_buffer *shm_buffer;
  cairo_surface_t *cr_surface = NULL;

  if (width_out)
    *width_out = -1;
  if (height_out)
    *height_out = -1;

  shm_buffer = wl_shm_buffer_get (surface->current.buffer);
  if (shm_buffer)
    {
      g_autoptr (WlShmBufferLocker) locked = wl_shm_buffer_locker (shm_buffer);
      cairo_format_t format;
      uint8_t *shm_pixels;

      shm_pixels = wl_shm_buffer_get_data (shm_buffer);
      format =
        cairo_format_for_wl_shm_format (wl_shm_buffer_get_format (shm_buffer));
      int width = wl_shm_buffer_get_width (shm_buffer);
      int height = wl_shm_buffer_get_height (shm_buffer);
      int shm_stride = wl_shm_buffer_get_stride (shm_buffer);
      int cr_stride;
      uint8_t *cr_pixels;
      int y;

      if (width_out)
        *width_out = width / surface->current.scale;
      if (height_out)
        *height_out = height / surface->current.scale;

      cr_surface = cairo_image_surface_create (format, width, height);
      cr_pixels = cairo_image_surface_get_data (cr_surface);
      cr_stride = cairo_image_surface_get_stride (cr_surface);
      for (y = 0; y < height; y++)
        {
          memcpy (cr_pixels + y * cr_stride,
                  shm_pixels + y * shm_stride,
                  MIN (cr_stride, shm_stride));
        }
      cairo_surface_set_device_scale (cr_surface,
                                      surface->current.scale,
                                      surface->current.scale);
      cairo_surface_mark_dirty (cr_surface);
    }

  return cr_surface;
}

void
wakefield_surface_draw (struct wl_resource *surface_resource,
                        cairo_t                 *cr)
{
  WakefieldSurface *surface = wl_resource_get_user_data (surface_resource);
  struct wl_shm_buffer *shm_buffer;

  shm_buffer = wl_shm_buffer_get (surface->current.buffer);
  if (shm_buffer)
    {
      g_autoptr (WlShmBufferLocker) locked = wl_shm_buffer_locker (shm_buffer);
      cairo_surface_t *cr_surface;

      cr_surface = cairo_image_surface_create_for_data (wl_shm_buffer_get_data (shm_buffer),
                                                        cairo_format_for_wl_shm_format (wl_shm_buffer_get_format (shm_buffer)),
                                                        wl_shm_buffer_get_width (shm_buffer),
                                                        wl_shm_buffer_get_height (shm_buffer),
                                                        wl_shm_buffer_get_stride (shm_buffer));
      cairo_surface_set_device_scale (cr_surface, surface->current.scale, surface->current.scale);

      if (surface->xdg_popup)
        {
          cairo_rectangle_int_t popup_allocation;

          wakefield_xdg_popup_get_allocation (surface->xdg_popup->resource,
                                              &popup_allocation);
          cairo_translate (cr, popup_allocation.x, popup_allocation.y);
        }

      cairo_set_source_surface (cr, cr_surface, 0, 0);

      /* XXX: Do scaling of our surface to match our allocation. */
      cairo_paint (cr);

      cairo_surface_destroy (cr_surface);
    }

  /* Trigger frame callbacks. */
  {
    struct wl_resource *cr, *next;
    int64_t now = g_get_monotonic_time () / 1000;

    wl_resource_for_each_safe (cr, next, &surface->current.frame_callbacks)
      {
        wl_callback_send_done (cr, now);
        wl_resource_destroy (cr);
      }

    wl_list_init (&surface->current.frame_callbacks);
  }
}

static void
wl_surface_destroy (struct wl_client *client,
                    struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
unbind_resource (struct wl_resource *resource)
{
  wl_list_remove (wl_resource_get_link (resource));
}


static void
wl_surface_attach (struct wl_client *client,
                   struct wl_resource *surface_resource,
                   struct wl_resource *buffer_resource,
                   gint32 dx, gint32 dy)
{
  WakefieldSurface *surface = wl_resource_get_user_data (surface_resource);

  /* Ignore dx/dy in our case */
  surface->pending.buffer = buffer_resource;
}

static void
wl_surface_damage (struct wl_client *client,
                   struct wl_resource *surface_resource,
                   int32_t x, int32_t y, int32_t width, int32_t height)
{
  WakefieldSurface *surface = wl_resource_get_user_data (surface_resource);
  cairo_rectangle_int_t rectangle = { x, y, width, height };
  cairo_region_union_rectangle (surface->damage, &rectangle);
}

#define WL_CALLBACK_VERSION 1

static void
wl_surface_frame (struct wl_client *client,
                  struct wl_resource *surface_resource,
                  uint32_t callback_id)
{
  WakefieldSurface *surface = wl_resource_get_user_data (surface_resource);

  struct wl_resource *callback = wl_resource_create (client, &wl_callback_interface,
                                                     WL_CALLBACK_VERSION, callback_id);
  wl_resource_set_destructor (callback, unbind_resource);
  wl_list_insert (&surface->pending.frame_callbacks, wl_resource_get_link (callback));
}

static void
wl_surface_set_opaque_region (struct wl_client *client,
                              struct wl_resource *surface_resource,
                              struct wl_resource *region_resource)
{
  /* XXX: Do we need this? */
}

static void
wl_surface_set_input_region (struct wl_client *client,
                             struct wl_resource *surface_resource,
                             struct wl_resource *region_resource)
{
  WakefieldSurface *surface = wl_resource_get_user_data (surface_resource);
  g_clear_pointer (&surface->pending.input_region, cairo_region_destroy);
  if (region_resource)
    {
      surface->pending.input_region = wakefield_region_get_region (region_resource);
    }
}

static void
xdg_popup_get_absolute_coordinates (struct wl_resource *xdg_popup_resource,
                                    GdkPoint           *point)
{
  WakefieldXdgPopup *xdg_popup = wl_resource_get_user_data (xdg_popup_resource);
  cairo_rectangle_int_t allocation;

  wakefield_xdg_popup_get_allocation (xdg_popup_resource, &allocation);
  point->x = allocation.x;
  point->y = allocation.y;

  while (xdg_popup->parent_surface &&
         xdg_popup->parent_surface->role == WAKEFIELD_SURFACE_ROLE_XDG_POPUP)
    {
      xdg_popup = xdg_popup->parent_surface->xdg_popup;
      wakefield_xdg_popup_get_allocation (xdg_popup->resource, &allocation);

      point->x += allocation.x;
      point->y += allocation.y;
    }
}

static void
wl_surface_commit (struct wl_client *client,
                   struct wl_resource *resource)
{
  WakefieldSurface *surface = wl_resource_get_user_data (resource);
  struct wl_shm_buffer *shm_buffer;
  cairo_region_t *clear_region = NULL;
  cairo_rectangle_int_t rect = { 0, };

  if (surface->current.buffer)
    {
      shm_buffer = wl_shm_buffer_get (surface->current.buffer);

      if (shm_buffer)
        {
          g_autoptr (WlShmBufferLocker) locked = wl_shm_buffer_locker (shm_buffer);

          rect.width = wl_shm_buffer_get_width (shm_buffer) / surface->current.scale;
          rect.height = wl_shm_buffer_get_height (shm_buffer) / surface->current.scale;

          clear_region = cairo_region_create_rectangle (&rect);
        }
    }

  if (surface->pending.buffer)
    {
      shm_buffer = wl_shm_buffer_get (surface->pending.buffer);

      if (clear_region && shm_buffer)
        {
          g_autoptr (WlShmBufferLocker) locked = wl_shm_buffer_locker (shm_buffer);

          rect.width = wl_shm_buffer_get_width (shm_buffer) /
                        surface->pending.scale;
          rect.height = wl_shm_buffer_get_height (shm_buffer) /
                         surface->pending.scale;

          cairo_region_subtract_rectangle (clear_region, &rect);
        }

      g_clear_pointer (&surface->current.buffer, wl_buffer_send_release);
      surface->current.buffer = g_steal_pointer (&surface->pending.buffer);
    }

  /* XXX: Should we reallocate / redraw the entire region if the buffer
   * scale changes? */
  if (surface->pending.scale > 0)
    surface->current.scale = surface->pending.scale;

  wl_list_insert_list (&surface->current.frame_callbacks,
                       &surface->pending.frame_callbacks);
  wl_list_init (&surface->pending.frame_callbacks);

  if (clear_region)
    {
      cairo_region_union (surface->damage, clear_region);
      cairo_region_destroy (clear_region);
    }

  /* process damage */

  if (surface->xdg_surface)
    {
      GtkAllocation allocation;

      gtk_widget_get_allocation (GTK_WIDGET (surface->compositor), &allocation);

      if (surface->xdg_popup)
        {
          GdkPoint popup_orig;

          xdg_popup_compute_allocation (surface->xdg_popup, TRUE);
          xdg_popup_get_absolute_coordinates (surface->xdg_popup->resource,
                                              &popup_orig);

          if (!cairo_region_intersect_rectangle (surface->damage, &allocation))
            {
              allocation.y += popup_orig.y;
              allocation.x += popup_orig.x;
            }

          if (surface->xdg_surface->window)
            {
              gdk_window_move (surface->xdg_surface->window,
                               popup_orig.x, popup_orig.y);
            }
        }

      cairo_region_translate (surface->damage, allocation.x, allocation.y);
      gtk_widget_queue_draw_region (GTK_WIDGET (surface->compositor), surface->damage);
    }

  /* ... and then empty it */
  {
    cairo_rectangle_int_t nothing = { 0, 0, 0, 0 };
    cairo_region_intersect_rectangle (surface->damage, &nothing);
  }

  /* XXX: Stop leak when we start using the input region. */
  surface->pending.input_region = NULL;

  surface->pending.scale = 1;

  if (!surface->mapped)
    {
      surface->mapped = TRUE;
      wakefield_compositor_surface_mapped (surface->compositor, surface->resource);
    }

  g_signal_emit (surface, signals[COMMITTED], 0);
}

static void
wl_surface_set_buffer_transform (struct wl_client *client,
                                 struct wl_resource *resource,
                                 int32_t transform)
{
  /* TODO */
}

static void
wl_surface_set_buffer_scale (struct wl_client *client,
                             struct wl_resource *resource,
                             int32_t scale)
{
  WakefieldSurface *surface = wl_resource_get_user_data (resource);
  surface->pending.scale = scale;
}

static void
destroy_pending_state (WakefieldSurfacePendingState *state)
{
  struct wl_resource *cr, *next;
  wl_resource_for_each_safe (cr, next, &state->frame_callbacks)
    wl_resource_destroy (cr);
  g_clear_pointer (&state->input_region, cairo_region_destroy);
}

/* This needs to be called both from wl_surface and xdg_[surface|popup] finalizer,
   because destructors are called in random order during client disconnect */
static void
wl_surface_unmap (WakefieldSurface *surface)
{
  if (surface->mapped)
    {
      surface->mapped = FALSE;
      wakefield_compositor_surface_unmapped (surface->compositor, surface->resource);
    }
}


static void
wl_surface_finalize (struct wl_resource *resource)
{
  WakefieldSurface *surface = wl_resource_get_user_data (resource);

  wl_surface_unmap (surface);

  if (surface->xdg_surface)
    surface->xdg_surface->surface = NULL;

  if (surface->xdg_toplevel)
    surface->xdg_toplevel->surface = NULL;

  if (surface->xdg_popup)
    surface->xdg_popup->surface = NULL;

  wl_list_remove (wl_resource_get_link (resource));

  destroy_pending_state (&surface->pending);
  destroy_pending_state (&surface->current);

  g_object_unref (surface);
}

static void
wl_surface_damage_buffer (struct wl_client *client,
                          struct wl_resource *resource,
                          int32_t x,
                          int32_t y,
                          int32_t width,
                          int32_t height)
{
  wl_resource_post_error (resource, 1,
                          "xdg-surface::damage_buffer not implemented yet.");
}

static void
wl_surface_offset (struct wl_client *client,
                   struct wl_resource *resource,
                   int32_t x,
                   int32_t y)
{
   wl_resource_post_error (resource, 1,
                           "xdg-surface::offset not implemented yet.");
}

static const struct wl_surface_interface surface_implementation = {
  wl_surface_destroy,
  wl_surface_attach,
  wl_surface_damage,
  wl_surface_frame,
  wl_surface_set_opaque_region,
  wl_surface_set_input_region,
  wl_surface_commit,
  wl_surface_set_buffer_transform,
  wl_surface_set_buffer_scale,
  wl_surface_damage_buffer,
  wl_surface_offset,
};

struct wl_resource *
wakefield_surface_new (WakefieldCompositor *compositor,
                       struct wl_client *client,
                       struct wl_resource *compositor_resource,
                       uint32_t id)
{
  WakefieldSurface *surface;

  surface = g_object_new (WAKEFIELD_TYPE_SURFACE, NULL);
  surface->compositor = compositor;
  surface->damage = cairo_region_create ();

  surface->resource = wl_resource_create (client, &wl_surface_interface, wl_resource_get_version (compositor_resource), id);
  wl_resource_set_implementation (surface->resource, &surface_implementation, surface, wl_surface_finalize);

  wl_list_init (&surface->pending.frame_callbacks);
  wl_list_init (&surface->current.frame_callbacks);

  surface->current.scale = 1;
  surface->pending.scale = 1;

  return surface->resource;
}

static void
xdg_surface_finalize (struct wl_resource *xdg_resource)
{
  WakefieldXdgSurface *xdg_surface = wl_resource_get_user_data (xdg_resource);

  wakefield_xdg_surface_unrealize (xdg_resource);

  wl_list_remove (wl_resource_get_link (xdg_resource));

  if (xdg_surface->surface)
    xdg_surface->surface->xdg_surface = NULL;

  g_free (xdg_surface);
}

static void
xdg_surface_destroy (struct wl_client *client,
                     struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
xdg_surface_set_window_geometry (struct wl_client *client,
                                 struct wl_resource *resource,
                                 int32_t x,
                                 int32_t y,
                                 int32_t width,
                                 int32_t height)
{
  WakefieldXdgSurface *xdg_surface = wl_resource_get_user_data (resource);

  if (xdg_surface->window)
    gdk_window_move_resize (xdg_surface->window, x, y, width, height);
}

static void
xdg_surface_ack_configure (struct wl_client *client,
                           struct wl_resource *resource,
                           uint32_t serial)
{
  // g_print("ACKIng configure %u\n", serial);
}

static const struct xdg_surface_interface xdg_surface_implementation = {
  .destroy = xdg_surface_destroy,
  .get_toplevel = xdg_surface_get_toplevel,
  .get_popup = xdg_surface_get_popup,
  .set_window_geometry = xdg_surface_set_window_geometry,
  .ack_configure = xdg_surface_ack_configure,
};

static void
xdg_toplevel_finalize (struct wl_resource *xdg_resource)
{
  WakefieldXdgToplevel *xdg_toplevel = wl_resource_get_user_data (xdg_resource);

  if (xdg_toplevel->surface)
    xdg_toplevel->surface->xdg_toplevel = NULL;

  g_free (xdg_toplevel);
}

static void
xdg_toplevel_destroy (struct wl_client *client,
                     struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
xdg_toplevel_set_parent (struct wl_client *client,
                         struct wl_resource *resource,
                         struct wl_resource *parent_resource)
{
}

static void
xdg_toplevel_set_app_id (struct wl_client *client,
                        struct wl_resource *resource,
                        const char *app_id)
{
}

static void
xdg_toplevel_show_window_menu (struct wl_client *client,
                               struct wl_resource *surface_resource,
                               struct wl_resource *seat_resource,
                               uint32_t serial,
                               int32_t x,
                               int32_t y)
{
}

static void
xdg_toplevel_set_title (struct wl_client *client,
                        struct wl_resource *resource, const char *title)
{
}

static void
xdg_toplevel_move (struct wl_client *client, struct wl_resource *resource,
                   struct wl_resource *seat_resource, uint32_t serial)
{
}

static void
xdg_toplevel_resize (struct wl_client *client, struct wl_resource *resource,
                     struct wl_resource *seat_resource, uint32_t serial,
                     uint32_t edges)
{
}

static void
xdg_toplevel_set_maximized (struct wl_client *client,
                            struct wl_resource *resource)
{
}

static void
xdg_toplevel_unset_maximized (struct wl_client *client,
                              struct wl_resource *resource)
{
}

static void
xdg_toplevel_set_fullscreen (struct wl_client *client,
                             struct wl_resource *resource,
                             struct wl_resource *output_resource)
{
}

static void
xdg_toplevel_unset_fullscreen (struct wl_client *client,
                               struct wl_resource *resource)
{
}

static void
xdg_toplevel_set_minimized (struct wl_client *client,
                            struct wl_resource *resource)
{
}

static void
xdg_toplevel_set_max_size (struct wl_client *client,
                           struct wl_resource *resource,
                           int32_t width,
                           int32_t height)
{
}

static void
xdg_toplevel_set_min_size (struct wl_client *client,
                           struct wl_resource *resource,
                           int32_t width,
                           int32_t height)
{
}

static const struct xdg_toplevel_interface xdg_toplevel_implementation = {
  .destroy = xdg_toplevel_destroy,
  .set_parent = xdg_toplevel_set_parent,
  .set_title = xdg_toplevel_set_title,
  .set_app_id = xdg_toplevel_set_app_id,
  .show_window_menu = xdg_toplevel_show_window_menu,
  .move = xdg_toplevel_move,
  .resize = xdg_toplevel_resize,
  .set_max_size = xdg_toplevel_set_max_size,
  .set_min_size = xdg_toplevel_set_min_size,
  .set_maximized = xdg_toplevel_set_maximized,
  .unset_maximized = xdg_toplevel_unset_maximized,
  .set_fullscreen = xdg_toplevel_set_fullscreen,
  .unset_fullscreen = xdg_toplevel_unset_fullscreen,
  .set_minimized = xdg_toplevel_set_minimized,
};

static void
xdg_surface_get_toplevel (struct wl_client   *client,
                          struct wl_resource *resource,
                          uint32_t id)
{
  WakefieldXdgToplevel *xdg_toplevel = g_new0 (WakefieldXdgToplevel, 1);
  WakefieldXdgSurface *xdg_surface = wl_resource_get_user_data (resource);
  WakefieldSurface *surface;

  g_assert (wl_resource_instance_of (resource, &xdg_surface_interface,
                                     &xdg_surface_implementation));

  surface = xdg_surface->surface;
  xdg_toplevel->surface = surface;

  g_assert (surface->xdg_toplevel == NULL);
  surface->xdg_toplevel = xdg_toplevel;

  xdg_toplevel->resource = wl_resource_create (client, &xdg_toplevel_interface,
                                               wl_resource_get_version (resource),
                                               id);

  if (xdg_toplevel->resource == NULL)
    {
      g_free (xdg_toplevel);
      surface->xdg_toplevel = NULL;
      wl_resource_post_no_memory (resource);
      return;
    }

  wakefield_surface_set_role (xdg_surface->surface->resource,
                              WAKEFIELD_SURFACE_ROLE_XDG_TOPLEVEL);

  wl_resource_set_implementation (xdg_toplevel->resource,
                                  &xdg_toplevel_implementation, xdg_toplevel,
                                  xdg_toplevel_finalize);

  wakefield_compositor_send_configure (
    wakefield_surface_get_compositor (surface), xdg_surface->resource);
}

WakefieldSurface *
wakefield_xdg_surface_get_surface (struct wl_resource *xdg_surface_resource)
{
  WakefieldXdgSurface *xdg_surface =
    wl_resource_get_user_data (xdg_surface_resource);

  g_return_val_if_fail (
    wl_resource_instance_of (xdg_surface_resource, &xdg_surface_interface,
                             &xdg_surface_implementation), NULL);

  return xdg_surface->surface;
}

struct wl_resource *
wakefield_xdg_surface_get_surface_resource (struct wl_resource *xdg_surface_resource)
{
  WakefieldSurface *surface =
    wakefield_xdg_surface_get_surface (xdg_surface_resource);

  if (surface)
    return surface->resource;

  return NULL;
}

GdkWindow *
wakefield_xdg_surface_get_window (struct wl_resource *xdg_surface_resource)
{
  WakefieldXdgSurface *xdg_surface = wl_resource_get_user_data (xdg_surface_resource);

  return xdg_surface->window;
}

void
wakefield_xdg_surface_realize (struct wl_resource *xdg_surface_resource,
                               GdkWindow *parent_window)
{
  WakefieldCompositor *compositor;
  WakefieldXdgSurface *xdg_surface = wl_resource_get_user_data (xdg_surface_resource);
  WakefieldSurface *surface = xdg_surface->surface;
  GdkWindowAttr attributes;
  gint attributes_mask;
  int width, height;

  if (surface == NULL)
    return;

  compositor = surface->compositor;

  wakefield_surface_get_current_size (xdg_surface->surface,
                                      &width, &height);

  attributes.x = 0;
  attributes.y = 0;
  attributes.width = width;
  attributes.height = height;
  attributes.wclass = GDK_INPUT_ONLY;
  attributes_mask = GDK_WA_X | GDK_WA_Y;
  attributes.window_type = GDK_WINDOW_CHILD;
  attributes.event_mask =
    GDK_POINTER_MOTION_MASK |
    GDK_BUTTON_PRESS_MASK |
    GDK_BUTTON_RELEASE_MASK |
    GDK_SCROLL_MASK |
    GDK_FOCUS_CHANGE_MASK |
    GDK_KEY_PRESS_MASK |
    GDK_KEY_RELEASE_MASK |
    GDK_ENTER_NOTIFY_MASK |
    GDK_LEAVE_NOTIFY_MASK;

  xdg_surface->window = gdk_window_new (parent_window, &attributes, attributes_mask);
  gtk_widget_register_window (GTK_WIDGET (compositor), xdg_surface->window);
  gdk_window_show (xdg_surface->window);
}

void
wakefield_xdg_surface_unrealize (struct wl_resource *xdg_surface_resource)
{
  WakefieldCompositor *compositor;
  WakefieldXdgSurface *xdg_surface = wl_resource_get_user_data (xdg_surface_resource);
  WakefieldSurface *surface = xdg_surface->surface;

  if (xdg_surface->surface)
    wl_surface_unmap (xdg_surface->surface);

  if (xdg_surface->window)
    {
      if (surface != NULL)
        {
          compositor = surface->compositor;
          gtk_widget_unregister_window (GTK_WIDGET (compositor), xdg_surface->window);
        }

      gdk_window_destroy (xdg_surface->window);
      xdg_surface->window = NULL;
    }
}

struct wl_resource *
wakefield_xdg_surface_new (struct wl_client *client,
                           struct wl_resource *shell_resource,
                           uint32_t id,
                           struct wl_resource *surface_resource)
{
  WakefieldSurface *surface = wl_resource_get_user_data (surface_resource);
  WakefieldXdgSurface *xdg_surface;

  xdg_surface = g_new0 (WakefieldXdgSurface, 1);
  xdg_surface->surface = surface;

  surface->xdg_surface = xdg_surface;

  xdg_surface->resource = wl_resource_create (client, &xdg_surface_interface, wl_resource_get_version (shell_resource), id);
  wl_resource_set_implementation (xdg_surface->resource, &xdg_surface_implementation, xdg_surface, xdg_surface_finalize);

  return xdg_surface->resource;
}

static void
xdg_popup_destroy (struct wl_client *client,
                   struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
get_positioner_anchor_point (WakeFieldXdgPositioner *xdg_positioner,
                             int32_t                *x,
                             int32_t                *y)
{
  cairo_rectangle_int_t *anchor_rect = &xdg_positioner->anchor_rect;

  switch (xdg_positioner->anchor)
    {
    case XDG_POSITIONER_ANCHOR_NONE:
      *x = anchor_rect->x + anchor_rect->width / 2;
      *y = anchor_rect->y + anchor_rect->height / 2;
      break;
    case XDG_POSITIONER_ANCHOR_TOP:
      *x = anchor_rect->x + anchor_rect->width / 2;
      *y = anchor_rect->y;
      break;
    case XDG_POSITIONER_ANCHOR_BOTTOM:
      *x = anchor_rect->x + anchor_rect->width / 2;
      *y = anchor_rect->y + anchor_rect->height;
      break;
    case XDG_POSITIONER_ANCHOR_LEFT:
      *x = anchor_rect->x;
      *y = anchor_rect->y + anchor_rect->height / 2;
      break;
    case XDG_POSITIONER_ANCHOR_RIGHT:
      *x = anchor_rect->x + anchor_rect->width;
      *y = anchor_rect->y + anchor_rect->height / 2;
      break;
    case XDG_POSITIONER_ANCHOR_TOP_LEFT:
      *x = anchor_rect->x;
      *y = anchor_rect->y;
      break;
    case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
      *x = anchor_rect->x;
      *y = anchor_rect->y + anchor_rect->height;
      break;
    case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
      *x = anchor_rect->x + anchor_rect->width;
      *y = anchor_rect->y;
      break;
    case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
      *x = anchor_rect->x + anchor_rect->width;
      *y = anchor_rect->y + anchor_rect->height;
      break;
    }
}

static WakefieldSurface *
get_parent_toplevel (WakefieldSurface *surface)
{
  if (surface->role == WAKEFIELD_SURFACE_ROLE_NONE)
    return NULL;

  if (surface->role == WAKEFIELD_SURFACE_ROLE_XDG_TOPLEVEL)
    return surface->xdg_toplevel->surface;

  return get_parent_toplevel (surface->xdg_popup->parent_surface);
}

static void
xdg_popup_compute_allocation (WakefieldXdgPopup *xdg_popup,
                              gboolean           use_surface_size)
{
  WakefieldCompositor *compositor = xdg_popup->surface->compositor;
  WakeFieldXdgPositioner *xdg_positioner;
  int32_t anchor_x, anchor_y;
  int32_t popup_width, popup_height;
  int32_t max_width, max_height;
  int32_t parent_width, parent_height;
  int surface_width, surface_height;
  int32_t x_offset, y_offset;

  xdg_positioner = &xdg_popup->xdg_positioner;

  if (use_surface_size &&
      xdg_popup->parent_surface->role == WAKEFIELD_SURFACE_ROLE_XDG_TOPLEVEL)
    {
      wakefield_surface_get_current_size (xdg_popup->surface,
                                          &surface_width, &surface_height);

      popup_width = MAX (xdg_positioner->width, surface_width);
      popup_height = MAX (xdg_positioner->height, surface_height);

      x_offset = MAX (0, popup_width - xdg_positioner->width) / 2;
      y_offset = MAX (0, popup_height - xdg_positioner->height) / 2;
    }
  else
    {
      x_offset = 0;
      y_offset = 0;
      popup_width = xdg_positioner->width;
      popup_height = xdg_positioner->height;
    }

  parent_width = 0;
  parent_height = 0;

  if (xdg_positioner->has_parent_size)
    {
      parent_width = xdg_positioner->parent_width;
      parent_height = xdg_positioner->parent_height;
    }
  else
    {
      WakefieldSurface *parent_toplevel =
        get_parent_toplevel (xdg_popup->surface);

      if (parent_toplevel && parent_toplevel->xdg_surface->window)
        {
          parent_width = gdk_window_get_width (
            parent_toplevel->xdg_surface->window);
          parent_height = gdk_window_get_height (
            parent_toplevel->xdg_surface->window);
        }
    }

  max_width = gtk_widget_get_allocated_width (GTK_WIDGET (compositor));
  max_height = gtk_widget_get_allocated_height (GTK_WIDGET (compositor));

  get_positioner_anchor_point (xdg_positioner, &anchor_x, &anchor_y);
  anchor_x = anchor_x - x_offset;
  anchor_y = anchor_y - y_offset;

  /* Try to keep the popup inside the parent */
  if (anchor_x + popup_width > parent_width &&
      (xdg_positioner->constraint_adjustment &
       XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X))
    anchor_x = MAX (0, parent_width - popup_width + x_offset);

  if (anchor_y + popup_height > parent_height &&
      xdg_positioner->constraint_adjustment &
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y)
    anchor_y = MAX (0, parent_height - popup_height + y_offset);

  /* Try to keep the popup inside the compositor area */
  if (anchor_x + popup_width > max_width &&
      (xdg_positioner->constraint_adjustment &
       XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X))
    anchor_x = MAX (0, max_width - popup_width + x_offset);

  if (anchor_y + popup_height > max_height &&
      xdg_positioner->constraint_adjustment &
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y)
    anchor_y = MAX (0, max_height - popup_height + y_offset);

  /* Resize the popup if nothing else was possible */
  if (anchor_x + popup_width > max_width &&
      (xdg_positioner->constraint_adjustment &
       XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X))
    popup_width = MAX (0, max_width - anchor_x);

  if (anchor_y + popup_height > max_height &&
      xdg_positioner->constraint_adjustment &
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y)
    popup_height = MAX (0, max_height - anchor_y);

  xdg_popup->allocation.x = MAX (0, anchor_x + xdg_positioner->offset_x);
  xdg_popup->allocation.y = MAX (0, anchor_y + xdg_positioner->offset_y);
  xdg_popup->allocation.width = popup_width;
  xdg_popup->allocation.height = popup_height;
}

static void
xdg_popup_grab (struct wl_client *client,
                struct wl_resource *resource,
                struct wl_resource *seat,
                uint32_t serial)
{
  WakefieldXdgPopup *xdg_popup = wl_resource_get_user_data (resource);
  xdg_popup->grab_serial = serial;

  wakefield_compositor_grab_pointer (
    wakefield_surface_get_compositor (xdg_popup->surface),
      xdg_popup->parent_surface->xdg_surface->resource,
      xdg_popup->surface->xdg_surface->resource, serial);
}

static void
xdg_popup_reposition (struct wl_client *client,
                      struct wl_resource *resource,
                      struct wl_resource *positioner,
                      uint32_t token)
{
  WakefieldXdgPopup *xdg_popup = wl_resource_get_user_data (resource);
  WakeFieldXdgPositioner *xdg_positioner = wl_resource_get_user_data (positioner);
  WakefieldSurface *surface = xdg_popup->surface;

  xdg_popup->xdg_positioner = *xdg_positioner;
  xdg_popup_compute_allocation (xdg_popup, FALSE);
  xdg_popup_send_repositioned (resource, token);

  wakefield_compositor_send_configure (
    wakefield_surface_get_compositor (surface), surface->xdg_surface->resource);
}

static const struct xdg_popup_interface xdg_popup_implementation = {
  .destroy = xdg_popup_destroy,
  .grab = xdg_popup_grab,
  .reposition = xdg_popup_reposition,
};

static void
xdg_popup_finalize (struct wl_resource *xdg_popup_resource)
{
  WakefieldXdgPopup *xdg_popup = wl_resource_get_user_data (xdg_popup_resource);

  if (xdg_popup->surface)
    wl_surface_unmap (xdg_popup->surface);

  if (xdg_popup->surface)
    xdg_popup->surface->xdg_popup = NULL;

  g_free (xdg_popup);
}

void
wakefield_xdg_popup_close (struct wl_resource *xdg_popup_resource)
{
  WakefieldXdgPopup *xdg_popup = wl_resource_get_user_data (xdg_popup_resource);
  WakefieldSurface *surface = xdg_popup->surface;

  if (surface && surface->mapped)
    {
      surface->mapped = FALSE;

      wakefield_compositor_surface_unmapped (surface->compositor, surface->resource);
    }

  xdg_popup_send_popup_done (xdg_popup_resource);
}

static void
xdg_surface_get_popup (struct wl_client *client,
                       struct wl_resource *resource,
                       uint32_t id,
                       struct wl_resource *parent,
                       struct wl_resource *positioner)
{
  WakefieldXdgPopup *xdg_popup = g_new0 (WakefieldXdgPopup, 1);
  WakefieldXdgSurface *xdg_surface = wl_resource_get_user_data (resource);
  WakeFieldXdgPositioner *xdg_positioner = wl_resource_get_user_data (positioner);
  WakefieldXdgSurface *parent_xdg_surface;
  WakefieldSurface *surface;

  g_assert (wl_resource_instance_of (resource, &xdg_surface_interface,
                                     &xdg_surface_implementation));

  if (!xdg_positioner)
    {
      wl_resource_post_error (resource, XDG_WM_BASE_ERROR_INVALID_POSITIONER,
                              "Missing popup positioner");
      return;
    }

  if (!parent)
    {
      wl_resource_post_error (resource, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                              "Invalid popup parent");
      return;
    }

  surface = xdg_surface->surface;
  xdg_popup->surface = surface;

  parent_xdg_surface = wl_resource_get_user_data (parent);
  xdg_popup->parent_surface = parent_xdg_surface->surface;

  g_assert (surface->xdg_popup == NULL);
  surface->xdg_popup = xdg_popup;

  xdg_popup->resource = wl_resource_create (client, &xdg_popup_interface,
                                            wl_resource_get_version (resource),
                                            id);

  if (xdg_popup->resource == NULL)
    {
      g_free (xdg_popup);
      surface->xdg_popup = NULL;
      wl_resource_post_no_memory (resource);
      return;
    }

  xdg_popup->xdg_positioner = *xdg_positioner;

  wakefield_surface_set_role (xdg_surface->surface->resource,
                              WAKEFIELD_SURFACE_ROLE_XDG_POPUP);

  wl_resource_set_implementation (xdg_popup->resource,
                                  &xdg_popup_implementation, xdg_popup,
                                  xdg_popup_finalize);

  xdg_popup_compute_allocation (xdg_popup, FALSE);

  wakefield_compositor_send_configure (
    wakefield_surface_get_compositor (surface), xdg_surface->resource);
}

void
wakefield_xdg_popup_get_allocation (struct wl_resource *xdg_popup_resource,
                                    cairo_rectangle_int_t *allocation)
{
  WakefieldXdgPopup *xdg_popup = wl_resource_get_user_data (xdg_popup_resource);

  g_assert (wl_resource_instance_of (xdg_popup_resource, &xdg_popup_interface,
                                     &xdg_popup_implementation));

  *allocation = xdg_popup->allocation;
}

static void
xdg_positioner_destroy (struct wl_client   *client,
                        struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
xdg_positioner_set_size (struct wl_client   *client,
                         struct wl_resource *resource,
                         int32_t             width,
                         int32_t             height)
{
  WakeFieldXdgPositioner *xdg_positioner = wl_resource_get_user_data (resource);

  if (width <= 0 || height <= 0)
    {
      wl_resource_post_error (resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
                              "Invalid size");
      return;
    }

  xdg_positioner->width = width;
  xdg_positioner->height = height;
}

static void
xdg_positioner_set_anchor_rect (struct wl_client   *client,
                                struct wl_resource *resource,
                                int32_t             x,
                                int32_t             y,
                                int32_t             width,
                                int32_t             height)
{
  WakeFieldXdgPositioner *xdg_positioner = wl_resource_get_user_data (resource);

  if (width <= 0 || height <= 0)
    {
      wl_resource_post_error (resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
                              "Invalid anchor rectangle size");
      return;
    }

  xdg_positioner->anchor_rect = (cairo_rectangle_int_t) {
    .x = x,
    .y = y,
    .width = width,
    .height = height,
  };
}

static void
xdg_positioner_set_anchor (struct wl_client   *client,
                           struct wl_resource *resource,
                           uint32_t            anchor)
{
  WakeFieldXdgPositioner *xdg_positioner = wl_resource_get_user_data (resource);

  if (anchor > XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT)
    {
      wl_resource_post_error (resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
                              "Invalid anchor");
      return;
    }

  xdg_positioner->anchor = anchor;
}

static void
xdg_positioner_set_gravity (struct wl_client   *client,
                            struct wl_resource *resource,
                            uint32_t            gravity)
{
  WakeFieldXdgPositioner *xdg_positioner = wl_resource_get_user_data (resource);

  if (gravity > XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT)
    {
      wl_resource_post_error (resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
                              "Invalid gravity");
      return;
    }

  xdg_positioner->gravity = gravity;
}

static void
xdg_positioner_set_constraint_adjustment (struct wl_client   *client,
                                          struct wl_resource *resource,
                                          uint32_t            constraint_adjustment)
{
  WakeFieldXdgPositioner *xdg_positioner = wl_resource_get_user_data (resource);
  uint32_t all_adjustments = (XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                              XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
                              XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
                              XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
                              XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X |
                              XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y);

  if ((constraint_adjustment & ~all_adjustments) != 0)
    {
      wl_resource_post_error (resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
                              "Invalid constraint action");
      return;
    }

  xdg_positioner->constraint_adjustment = constraint_adjustment;
}

static void
xdg_positioner_set_offset (struct wl_client   *client,
                           struct wl_resource *resource,
                           int32_t             x,
                           int32_t             y)
{
  WakeFieldXdgPositioner *xdg_positioner = wl_resource_get_user_data (resource);

  xdg_positioner->offset_x = x;
  xdg_positioner->offset_y = y;
}

static void
xdg_positioner_set_reactive (struct wl_client   *client,
                             struct wl_resource *resource)
{
  WakeFieldXdgPositioner *xdg_positioner = wl_resource_get_user_data (resource);

  xdg_positioner->is_reactive = TRUE;
}

static void
xdg_positioner_set_parent_size (struct wl_client   *client,
                                struct wl_resource *resource,
                                int32_t             parent_width,
                                int32_t             parent_height)
{
  WakeFieldXdgPositioner *xdg_positioner = wl_resource_get_user_data (resource);

  xdg_positioner->has_parent_size = TRUE;
  xdg_positioner->parent_width = parent_width;
  xdg_positioner->parent_height = parent_height;
}

static void
xdg_positioner_set_parent_configure (struct wl_client   *client,
                                     struct wl_resource *resource,
                                     uint32_t            serial)
{
  WakeFieldXdgPositioner *xdg_positioner = wl_resource_get_user_data (resource);

  xdg_positioner->acked_parent_configure = TRUE;
  xdg_positioner->parent_configure_serial = serial;
}

static const struct xdg_positioner_interface xdg_positioner_implementation = {
  .destroy = xdg_positioner_destroy,
  .set_size = xdg_positioner_set_size,
  .set_anchor_rect = xdg_positioner_set_anchor_rect,
  .set_anchor = xdg_positioner_set_anchor,
  .set_gravity = xdg_positioner_set_gravity,
  .set_constraint_adjustment = xdg_positioner_set_constraint_adjustment,
  .set_offset = xdg_positioner_set_offset,
  .set_reactive = xdg_positioner_set_reactive,
  .set_parent_size = xdg_positioner_set_parent_size,
  .set_parent_configure = xdg_positioner_set_parent_configure,
};

static void
xdg_positioner_finalize (struct wl_resource *resource)
{
  WakeFieldXdgPositioner *xdg_positioner = wl_resource_get_user_data (resource);

  g_free (xdg_positioner);
}

struct wl_resource *
wakefield_xdg_positioner_new (struct wl_client *client,
                              struct wl_resource *resource,
                              uint32_t id)
{
  WakeFieldXdgPositioner *xdg_positioner;
  struct wl_resource *positioner_resource;

  xdg_positioner = g_new0 (WakeFieldXdgPositioner, 1);
  positioner_resource = wl_resource_create (client,
                                            &xdg_positioner_interface,
                                            wl_resource_get_version (resource),
                                            id);
  wl_resource_set_implementation (positioner_resource,
                                  &xdg_positioner_implementation,
                                  xdg_positioner,
                                  xdg_positioner_finalize);
  return positioner_resource;
}

static void
wakefield_surface_init (WakefieldSurface *surface)
{
}

static void
wakefield_surface_class_init (WakefieldSurfaceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  signals[COMMITTED] = g_signal_new ("committed",
                                     G_TYPE_FROM_CLASS (object_class),
                                     G_SIGNAL_RUN_FIRST,
                                     0,
                                     NULL, NULL, NULL,
                                     G_TYPE_NONE, 0);
}
