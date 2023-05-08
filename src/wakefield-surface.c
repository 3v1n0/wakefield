/*
 * Copyright (C) 2015 Endless OS Foundation LLC
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
 */

#include "config.h"

#include <string.h>

#include "wakefield-private.h"
#include "xdg-shell-server-protocol.h"

#define WAKEFIELD_TYPE_SURFACE            (wakefield_surface_get_type ())
#define WAKEFIELD_SURFACE(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), WAKEFIELD_TYPE_SURFACE, WakefieldSurface))
#define WAKEFIELD_SURFACE_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  WAKEFIELD_TYPE_SURFACE, WakefieldSurfaceClass))
#define WAKEFIELD_IS_SURFACE(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), WAKEFIELD_TYPE_SURFACE))
#define WAKEFIELD_IS_SURFACE_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  WAKEFIELD_TYPE_SURFACE))
#define WAKEFIELD_SURFACE_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  WAKEFIELD_TYPE_SURFACE, WakefieldSurfaceClass))

typedef struct _WakefieldSurfaceClass WakefieldSurfaceClass;

struct _WakefieldSurfaceClass
{
  GObjectClass parent_class;
};

GType wakefield_surface_get_type (void) G_GNUC_CONST;

enum {
  COMMITTED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

struct WakefieldSurfacePendingState
{
  struct wl_resource *buffer;
  int scale;

  cairo_region_t *input_region;
  struct wl_list frame_callbacks;
};

struct _WakefieldSurface
{
  GObject parent;

  WakefieldCompositor *compositor;
  struct wl_resource *resource;

  WakefieldSurfaceRole role;

  struct WakefieldXdgSurface *xdg_surface;
  struct WakefieldXdgPopup *xdg_popup;

  cairo_region_t *damage;
  struct WakefieldSurfacePendingState pending, current;
  gboolean mapped;
};

struct WakefieldXdgSurface
{
  WakefieldSurface *surface;

  struct wl_resource *resource;
  GdkWindow *window;
};

struct WakefieldXdgPopup
{
  WakefieldSurface *surface;
  WakefieldSurface *parent_surface;

  GtkWidget *toplevel;
  GtkWidget *drawing_area;
  int x, y;
  guint32 serial;

  struct wl_resource *resource;
};

G_DEFINE_TYPE (WakefieldSurface, wakefield_surface, G_TYPE_OBJECT);

struct wl_resource *
wakefield_surface_get_xdg_surface  (struct wl_resource  *surface_resource)
{
  WakefieldSurface *surface = wl_resource_get_user_data (surface_resource);

  if (surface->xdg_surface)
    return surface->xdg_surface->resource;
  return NULL;
}

struct wl_resource *
wakefield_surface_get_xdg_popup  (struct wl_resource  *surface_resource)
{
  WakefieldSurface *surface = wl_resource_get_user_data (surface_resource);

  if (surface->xdg_popup)
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

  if (surface->xdg_popup)
    return wakefield_xdg_popup_get_window (surface->xdg_popup->resource);

  return NULL;
}

static void
wakefield_surface_get_current_size (WakefieldSurface *surface,
                                    int *width, int *height)
{
  struct wl_shm_buffer *shm_buffer;

  *width = 0;
  *height = 0;

  shm_buffer = wl_shm_buffer_get (surface->current.buffer);
  if (shm_buffer)
    {
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
      uint8_t *shm_pixels = wl_shm_buffer_get_data (shm_buffer);
      cairo_format_t format =
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
      wl_shm_buffer_begin_access (shm_buffer);
      for (y = 0; y < height; y++)
        {
          memcpy (cr_pixels + y * cr_stride,
                  shm_pixels + y * shm_stride,
                  MIN (cr_stride, shm_stride));
        }
      wl_shm_buffer_end_access (shm_buffer);
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
      cairo_surface_t *cr_surface;

      wl_shm_buffer_begin_access (shm_buffer);

      cr_surface = cairo_image_surface_create_for_data (wl_shm_buffer_get_data (shm_buffer),
                                                        cairo_format_for_wl_shm_format (wl_shm_buffer_get_format (shm_buffer)),
                                                        wl_shm_buffer_get_width (shm_buffer),
                                                        wl_shm_buffer_get_height (shm_buffer),
                                                        wl_shm_buffer_get_stride (shm_buffer));
      cairo_surface_set_device_scale (cr_surface, surface->current.scale, surface->current.scale);

      cairo_set_source_surface (cr, cr_surface, 0, 0);

      /* XXX: Do scaling of our surface to match our allocation. */
      cairo_paint (cr);

      cairo_surface_destroy (cr_surface);

      wl_shm_buffer_end_access (shm_buffer);
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
wl_surface_commit (struct wl_client *client,
                   struct wl_resource *resource)
{
  WakefieldSurface *surface = wl_resource_get_user_data (resource);
  struct wl_shm_buffer *shm_buffer;
  cairo_region_t *clear_region = NULL;
  cairo_rectangle_int_t rect = { 0, };
  int new_width = 0, new_height = 0;

  if (surface->current.buffer)
    {
      shm_buffer = wl_shm_buffer_get (surface->current.buffer);
      if (shm_buffer)
        {
          rect.width = wl_shm_buffer_get_width (shm_buffer) / surface->current.scale;
          rect.height = wl_shm_buffer_get_height (shm_buffer) / surface->current.scale;

          clear_region = cairo_region_create_rectangle (&rect);
        }
      wl_buffer_send_release (surface->current.buffer);
    }

  if (surface->pending.buffer)
    {
      shm_buffer = wl_shm_buffer_get (surface->pending.buffer);
      new_width = wl_shm_buffer_get_width (shm_buffer) / surface->pending.scale;
      new_height = wl_shm_buffer_get_height (shm_buffer) / surface->pending.scale;
      if (clear_region && shm_buffer)
        {
          rect.width = new_width;
          rect.height = new_height;

          cairo_region_subtract_rectangle (clear_region, &rect);
        }
      surface->current.buffer = surface->pending.buffer;
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

      cairo_region_translate (surface->damage, allocation.x, allocation.y);
      gtk_widget_queue_draw_region (GTK_WIDGET (surface->compositor), surface->damage);

      if (surface->xdg_surface->window)
        gdk_window_resize (surface->xdg_surface->window,
                           new_width,
                           new_height);
    }
  else if (surface->xdg_popup && new_width > 0 && new_height > 0)
    {
      struct WakefieldXdgPopup *xdg_popup = surface->xdg_popup;
      gint root_x, root_y;

      gtk_widget_set_size_request (GTK_WIDGET (xdg_popup->drawing_area), new_width, new_height);
      gtk_window_resize (GTK_WINDOW (xdg_popup->toplevel), new_width, new_height);

      if (!surface->mapped)
        {
          GdkWindow *parent_window = wakefield_surface_get_window (xdg_popup->parent_surface->resource);

          gdk_window_get_root_coords (parent_window, 0, 0, &root_x, &root_y);

          gtk_window_move (GTK_WINDOW (xdg_popup->toplevel),
                           root_x + xdg_popup->x, root_y + xdg_popup->y);
          gtk_widget_show (xdg_popup->toplevel);
        }

      gtk_widget_queue_draw_region (GTK_WIDGET (xdg_popup->drawing_area), surface->damage);
    }

  /* ... and then empty it */
  {
    cairo_rectangle_int_t nothing = { 0, 0, 0, 0 };
    cairo_region_intersect_rectangle (surface->damage, &nothing);
  }

  /* XXX: Stop leak when we start using the input region. */
  surface->pending.input_region = NULL;

  surface->pending.buffer = NULL;
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
destroy_pending_state (struct WakefieldSurfacePendingState *state)
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
  struct WakefieldXdgSurface *xdg_surface = wl_resource_get_user_data (xdg_resource);

  wakefield_xdg_surface_unrealize (xdg_resource);

  wl_list_remove (wl_resource_get_link (xdg_resource));

  if (xdg_surface->surface)
    xdg_surface->surface->xdg_surface = NULL;

  g_slice_free (struct WakefieldXdgSurface, xdg_surface);
}

static void
xdg_surface_destroy (struct wl_client *client,
                     struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
xdg_surface_set_parent (struct wl_client *client,
                        struct wl_resource *resource,
                        struct wl_resource *parent_resource)
{
}

static void
xdg_surface_set_app_id (struct wl_client *client,
                        struct wl_resource *resource,
                        const char *app_id)
{
}

static void
xdg_surface_show_window_menu (struct wl_client *client,
                              struct wl_resource *surface_resource,
                              struct wl_resource *seat_resource,
                              uint32_t serial,
                              int32_t x,
                              int32_t y)
{
}

static void
xdg_surface_set_title (struct wl_client *client,
                       struct wl_resource *resource, const char *title)
{
}

static void
xdg_surface_move (struct wl_client *client, struct wl_resource *resource,
                  struct wl_resource *seat_resource, uint32_t serial)
{
}

static void
xdg_surface_resize (struct wl_client *client, struct wl_resource *resource,
                    struct wl_resource *seat_resource, uint32_t serial,
                    uint32_t edges)
{
}

static void
xdg_surface_ack_configure (struct wl_client *client,
                           struct wl_resource *resource,
                           uint32_t serial)
{
}

static void
xdg_surface_set_window_geometry (struct wl_client *client,
                                 struct wl_resource *resource,
                                 int32_t x,
                                 int32_t y,
                                 int32_t width,
                                 int32_t height)
{
}

static void
xdg_surface_set_maximized (struct wl_client *client,
                           struct wl_resource *resource)
{
}

static void
xdg_surface_unset_maximized (struct wl_client *client,
                             struct wl_resource *resource)
{
}

static void
xdg_surface_set_fullscreen (struct wl_client *client,
                            struct wl_resource *resource,
                            struct wl_resource *output_resource)
{
}

static void
xdg_surface_unset_fullscreen (struct wl_client *client,
                              struct wl_resource *resource)
{
}

static void
xdg_surface_set_minimized (struct wl_client *client,
                           struct wl_resource *resource)
{
}

static const struct xdg_surface_interface xdg_surface_implementation = {
  xdg_surface_destroy,
  xdg_surface_set_parent,
  xdg_surface_set_title,
  xdg_surface_set_app_id,
  xdg_surface_show_window_menu,
  xdg_surface_move,
  xdg_surface_resize,
  xdg_surface_ack_configure,
  xdg_surface_set_window_geometry,
  xdg_surface_set_maximized,
  xdg_surface_unset_maximized,
  xdg_surface_set_fullscreen,
  xdg_surface_unset_fullscreen,
  xdg_surface_set_minimized,
};

struct wl_resource *
wakefield_xdg_surface_get_surface (struct wl_resource *xdg_surface_resource)
{
  struct WakefieldXdgSurface *xdg_surface = wl_resource_get_user_data (xdg_surface_resource);

  if (xdg_surface->surface)
    return xdg_surface->surface->resource;

  return NULL;
}

GdkWindow *
wakefield_xdg_surface_get_window (struct wl_resource *xdg_surface_resource)
{
  struct WakefieldXdgSurface *xdg_surface = wl_resource_get_user_data (xdg_surface_resource);

  return xdg_surface->window;
}

void
wakefield_xdg_surface_realize (struct wl_resource *xdg_surface_resource,
                               GdkWindow *parent_window)
{
  WakefieldCompositor *compositor;
  struct WakefieldXdgSurface *xdg_surface = wl_resource_get_user_data (xdg_surface_resource);
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
  struct WakefieldXdgSurface *xdg_surface = wl_resource_get_user_data (xdg_surface_resource);
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
  struct WakefieldXdgSurface *xdg_surface;

  wakefield_surface_set_role (surface_resource,
                              WAKEFIELD_SURFACE_ROLE_XDG_SURFACE);

  xdg_surface = g_slice_new0 (struct WakefieldXdgSurface);
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

static const struct xdg_popup_interface xdg_popup_implementation = {
  xdg_popup_destroy,
};

static void
xdg_popup_finalize (struct wl_resource *xdg_popup_resource)
{
  struct WakefieldXdgPopup *xdg_popup = wl_resource_get_user_data (xdg_popup_resource);

  if (xdg_popup->surface)
    wl_surface_unmap (xdg_popup->surface);

  wl_list_remove (wl_resource_get_link (xdg_popup_resource));

  if (xdg_popup->surface)
    xdg_popup->surface->xdg_popup = NULL;

  gtk_widget_destroy (xdg_popup->toplevel);

  g_slice_free (struct WakefieldXdgPopup, xdg_popup);
}

static gboolean
xdg_popup_draw (GtkWidget *widget,
                cairo_t   *cr,
                struct WakefieldXdgPopup *xdg_popup)
{
  if (xdg_popup->surface)
    wakefield_surface_draw (xdg_popup->surface->resource, cr);
  return TRUE;
}

static gboolean
xdg_popup_enter_notify (GtkWidget        *widget,
                        GdkEventCrossing *event,
                        struct WakefieldXdgPopup *xdg_popup)
{
  if (event->mode == GDK_CROSSING_NORMAL && xdg_popup->surface)
    wakefield_compositor_send_enter (xdg_popup->surface->compositor,
                                     xdg_popup->surface->resource,
                                     event);

  return FALSE;
}

static gboolean
xdg_popup_leave_notify (GtkWidget        *widget,
                        GdkEventCrossing *event,
                        struct WakefieldXdgPopup *xdg_popup)
{
  if (event->mode == GDK_CROSSING_NORMAL && xdg_popup->surface)
    wakefield_compositor_send_leave (xdg_popup->surface->compositor,
                                     xdg_popup->surface->resource,
                                     event);

  return FALSE;
}

static gboolean
xdg_popup_motion_notify (GtkWidget        *widget,
                         GdkEventMotion   *event,
                         struct WakefieldXdgPopup *xdg_popup)
{
  if (xdg_popup->surface)
    wakefield_compositor_send_motion (xdg_popup->surface->compositor,
                                      xdg_popup->surface->resource,
                                      event);

  return FALSE;
}

static gboolean
xdg_popup_button_press_event (GtkWidget      *widget,
                              GdkEventButton *event,
                              struct WakefieldXdgPopup *xdg_popup)
{
  if (xdg_popup->surface)
    wakefield_compositor_send_button (xdg_popup->surface->compositor,
                                      xdg_popup->surface->resource,
                                      event);
  return TRUE;
}

static gboolean
xdg_popup_button_release_event (GtkWidget      *widget,
                                GdkEventButton *event,
                                struct WakefieldXdgPopup *xdg_popup)
{
  if (xdg_popup->surface)
    wakefield_compositor_send_button (xdg_popup->surface->compositor,
                                      xdg_popup->surface->resource,
                                      event);
  return TRUE;
}

static gboolean
xdg_popup_scroll_event (GtkWidget      *widget,
                        GdkEventScroll *event,
                        struct WakefieldXdgPopup *xdg_popup)
{
  if (xdg_popup->surface)
    wakefield_compositor_send_scroll (xdg_popup->surface->compositor,
                                      xdg_popup->surface->resource,
                                      event);
  return TRUE;
}

guint32
wakefield_xdg_popup_get_serial (struct wl_resource *xdg_popup_resource)
{
  struct WakefieldXdgPopup *xdg_popup = wl_resource_get_user_data (xdg_popup_resource);

  return xdg_popup->serial;
}

GdkWindow *
wakefield_xdg_popup_get_window (struct wl_resource *xdg_popup_resource)
{
  struct WakefieldXdgPopup *xdg_popup = wl_resource_get_user_data (xdg_popup_resource);

  return gtk_widget_get_window (xdg_popup->drawing_area);
}

void
wakefield_xdg_popup_close (struct wl_resource *xdg_popup_resource)
{
  struct WakefieldXdgPopup *xdg_popup = wl_resource_get_user_data (xdg_popup_resource);
  WakefieldSurface *surface = xdg_popup->surface;

  if (surface && surface->mapped)
    {
      gtk_widget_hide (xdg_popup->toplevel);
      surface->mapped = FALSE;

      wakefield_compositor_surface_unmapped (surface->compositor, surface->resource);
    }

  xdg_popup_send_popup_done (xdg_popup_resource);
}

static GdkWindow *
get_toplevel (WakefieldSurface *surface)
{
  if (surface->xdg_popup)
    return gtk_widget_get_window (surface->xdg_popup->toplevel);
  else
    return surface->xdg_surface->window;
}

struct wl_resource *
wakefield_xdg_popup_new (WakefieldCompositor *compositor,
                         struct wl_client   *client,
                         struct wl_resource *shell_resource,
                         uint32_t            id,
                         struct wl_resource *surface_resource,
                         struct wl_resource *parent_resource,
                         guint32 serial,
                         gint32 x, gint32 y)
{
  WakefieldSurface *surface = wl_resource_get_user_data (surface_resource);
  WakefieldSurface *parent_surface = wl_resource_get_user_data (parent_resource);
  struct WakefieldXdgPopup *xdg_popup;
  GdkWindow *popup_window;
  GdkScreen *screen = gtk_widget_get_screen (GTK_WIDGET (compositor));
  GdkVisual *rgba_visual = gdk_screen_get_rgba_visual (screen);

  wakefield_surface_set_role (surface_resource,
                              WAKEFIELD_SURFACE_ROLE_XDG_SURFACE);

  xdg_popup = g_slice_new0 (struct WakefieldXdgPopup);
  xdg_popup->surface = surface;
  xdg_popup->parent_surface = parent_surface;

  xdg_popup->toplevel = gtk_window_new (GTK_WINDOW_POPUP);
  gtk_window_set_screen (GTK_WINDOW (xdg_popup->toplevel), screen);
  if (rgba_visual)
    gtk_widget_set_visual (GTK_WIDGET (xdg_popup->toplevel), rgba_visual);

  gtk_widget_set_app_paintable (GTK_WIDGET (xdg_popup->toplevel), TRUE);
  gtk_widget_realize (xdg_popup->toplevel);
  popup_window = gtk_widget_get_window (xdg_popup->toplevel);
  gdk_window_set_transient_for (popup_window, get_toplevel (parent_surface));
  gdk_window_set_type_hint (popup_window, GDK_WINDOW_TYPE_HINT_POPUP_MENU);

  xdg_popup->drawing_area = gtk_drawing_area_new ();
  gtk_widget_set_events (xdg_popup->drawing_area,
                         GDK_POINTER_MOTION_MASK |
                         GDK_BUTTON_PRESS_MASK |
                         GDK_BUTTON_RELEASE_MASK |
                         GDK_SCROLL_MASK |
                         GDK_FOCUS_CHANGE_MASK |
                         GDK_KEY_PRESS_MASK |
                         GDK_KEY_RELEASE_MASK |
                         GDK_ENTER_NOTIFY_MASK |
                         GDK_LEAVE_NOTIFY_MASK |
                         GDK_EXPOSURE_MASK);
  gtk_container_add (GTK_CONTAINER (xdg_popup->toplevel), xdg_popup->drawing_area);
  gtk_widget_show (xdg_popup->drawing_area);
  xdg_popup->serial = serial;
  xdg_popup->x = x;
  xdg_popup->y = y;

  g_signal_connect (xdg_popup->drawing_area, "draw", G_CALLBACK (xdg_popup_draw),
                    xdg_popup);
  g_signal_connect (xdg_popup->drawing_area, "enter-notify-event", G_CALLBACK (xdg_popup_enter_notify),
                    xdg_popup);
  g_signal_connect (xdg_popup->drawing_area, "leave-notify-event", G_CALLBACK (xdg_popup_leave_notify),
                    xdg_popup);
  g_signal_connect (xdg_popup->drawing_area, "motion-notify-event", G_CALLBACK (xdg_popup_motion_notify),
                    xdg_popup);
  g_signal_connect (xdg_popup->drawing_area, "button-press-event", G_CALLBACK (xdg_popup_button_press_event),
                    xdg_popup);
  g_signal_connect (xdg_popup->drawing_area, "button-release-event", G_CALLBACK (xdg_popup_button_release_event),
                    xdg_popup);
  g_signal_connect (xdg_popup->drawing_area, "scroll-event", G_CALLBACK (xdg_popup_scroll_event),
                    xdg_popup);

  surface->xdg_popup = xdg_popup;

  xdg_popup->resource = wl_resource_create (client, &xdg_popup_interface, wl_resource_get_version (shell_resource), id);
  wl_resource_set_implementation (xdg_popup->resource, &xdg_popup_implementation, xdg_popup, xdg_popup_finalize);

  return xdg_popup->resource;
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
