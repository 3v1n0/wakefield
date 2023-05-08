/*
 * Copyright (C) 2015 Endless OS Foundation LLC
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by:
 *     Jasper St. Pierre <jstpierre@mecheye.net>
 *     Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>

#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include "wakefield-compositor.h"
#include "wakefield-private.h"
#include "xdg-shell-server-protocol.h"

#include <xkbcommon/xkbcommon.h>

#if defined(GDK_WINDOWING_X11)
#include <xkbcommon/xkbcommon-x11.h>
#include <gdk/gdkx.h>
#include <X11/Xlib-xcb.h>
#endif

struct WakefieldPointer
{
  struct wl_list resource_list;

  guint32 serial;
  guint32 button_count;

  /* This is set to the surface that has been sent the enter notify (and no leave).
     Typically its the one with the pointer, except its always the grabbed surface
     during an implicit grab */
  struct wl_resource *current_surface;
  /* This is what we got told by gdk about the current state. The difference is
     that we don't forward enter/leave events during an implicit grab */
  struct wl_resource *current_gdk_surface;

  WakefieldSurface *cursor_surface;
  gulong cursor_surface_commit_listener;
  int hot_x;
  int hot_y;

  struct wl_client *grab_client;
  guint32 grab_button;
  GdkDevice *grab_device;
  GdkWindow *grab_window;
  struct wl_resource *grab_initial_surface;
  struct wl_resource *grab_popup_surface;
  guint32 grab_time;
  guint32 grab_serial;
};

struct WakefieldKeyboard
{
  struct wl_list resource_list;
  struct wl_resource *focus;
  struct xkb_context *context;
  struct xkb_keymap *keymap;
  int keymap_fd;
  gsize keymap_size;
  gboolean has_x11_xkb;
  xkb_mod_index_t shift_mod;
  xkb_mod_index_t caps_mod;
  xkb_mod_index_t ctrl_mod;
  xkb_mod_index_t alt_mod;
  xkb_mod_index_t mod2_mod;
  xkb_mod_index_t mod3_mod;
  xkb_mod_index_t super_mod;
  xkb_mod_index_t mod5_mod;
  xkb_led_index_t num_led;
  xkb_led_index_t caps_led;
  xkb_led_index_t scroll_led;

  guint32 mods_depressed;
  guint32 mods_latched;
  guint32 mods_locked;
  guint32 group;
};

struct WakefieldOutput
{
  struct wl_list resource_list;
};

struct WakefieldSeat
{
  struct WakefieldPointer pointer;
  struct WakefieldKeyboard keyboard;
};

struct WakefieldRegion
{
  struct wl_resource *resource;
  cairo_region_t *region;
};

struct _WakefieldCompositorPrivate
{
  GdkWindow *event_window;
  GSource *wayland_source;
  struct wl_display *wl_display;

  struct wl_list surfaces;
  struct wl_list xdg_surfaces;
  struct wl_list xdg_popups;
  struct wl_list shell_resources;
  struct WakefieldSeat seat;
  struct WakefieldOutput output;
  struct WakefieldDataDevice *data_device;
};
typedef struct _WakefieldCompositorPrivate WakefieldCompositorPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (WakefieldCompositor, wakefield_compositor, GTK_TYPE_WIDGET);

#define wl_resource_for_each_reverse(resource, list)                   \
	for (resource = 0, resource = wl_resource_from_link((list)->prev);	\
	     wl_resource_get_link(resource) != (list);				\
	     resource = wl_resource_from_link(wl_resource_get_link(resource)->prev))

static void
unset_cursor_surface (struct WakefieldPointer *pointer,
                      WakefieldSurface *cursor_surface);

static void
wakefield_compositor_realize (GtkWidget *widget)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  GtkAllocation allocation;
  GdkWindow *window;
  GdkWindowAttr attributes;
  gint attributes_mask;
  struct wl_resource *xdg_surface_resource;

  gtk_widget_set_realized (widget, TRUE);

  window = gtk_widget_get_parent_window (widget);
  gtk_widget_set_window (widget, window);
  g_object_ref (window);

  gtk_widget_get_allocation (widget, &allocation);

  attributes.x = allocation.x;
  attributes.y = allocation.y;
  attributes.width = allocation.width;
  attributes.height = allocation.height;
  attributes.wclass = GDK_INPUT_ONLY;
  attributes.visual = gtk_widget_get_visual (widget);
  attributes_mask = GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL;

  attributes.window_type = GDK_WINDOW_CHILD;

  attributes.event_mask = (gtk_widget_get_events (widget) |
                           GDK_EXPOSURE_MASK);

  priv->event_window = gdk_window_new (window,
                                       &attributes, attributes_mask);
  gtk_widget_register_window (widget, priv->event_window);

  wl_resource_for_each (xdg_surface_resource, &priv->xdg_surfaces)
    {
      wakefield_xdg_surface_realize (xdg_surface_resource, priv->event_window);
    }
}

static void
wakefield_compositor_unrealize (GtkWidget *widget)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct wl_resource *xdg_surface_resource;

  wl_resource_for_each (xdg_surface_resource, &priv->xdg_surfaces)
    {
      wakefield_xdg_surface_unrealize (xdg_surface_resource);
    }

  if (priv->event_window != NULL)
    {
      gtk_widget_unregister_window (widget, priv->event_window);
      gdk_window_destroy (priv->event_window);
      priv->event_window = NULL;
    }

  GTK_WIDGET_CLASS (wakefield_compositor_parent_class)->unrealize (widget);
}


static void
wakefield_compositor_map (GtkWidget *widget)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);

  GTK_WIDGET_CLASS (wakefield_compositor_parent_class)->map (widget);

  gdk_window_show (priv->event_window);
}

static void
wakefield_compositor_unmap (GtkWidget *widget)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);

  gdk_window_hide (priv->event_window);

  GTK_WIDGET_CLASS (wakefield_compositor_parent_class)->unmap (widget);
}

static void
refresh_output (WakefieldCompositor *compositor,
                struct wl_resource *output)
{
  GtkAllocation allocation;

  gtk_widget_get_allocation (GTK_WIDGET (compositor), &allocation);

  wl_output_send_scale (output, gtk_widget_get_scale_factor (GTK_WIDGET (compositor)));
  wl_output_send_mode (output,
                       WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                       allocation.width,
                       allocation.height,
                       60);
  wl_output_send_done (output);
}

static void
send_xdg_configure_request (WakefieldCompositor *compositor,
                            struct wl_resource *xdg_surface)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  GtkAllocation allocation;
  struct wl_array states;
  uint32_t serial = wl_display_next_serial (priv->wl_display);
  uint32_t *s;

  gtk_widget_get_allocation (GTK_WIDGET (compositor), &allocation);

  wl_array_init(&states);
  s = wl_array_add(&states, sizeof *s);
  *s = XDG_SURFACE_STATE_FULLSCREEN;
  if ((gtk_widget_get_state_flags (GTK_WIDGET (compositor)) & GTK_STATE_FLAG_BACKDROP) == 0)
    {
      s = wl_array_add(&states, sizeof *s);
      *s = XDG_SURFACE_STATE_ACTIVATED;
    }
  xdg_surface_send_configure (xdg_surface, allocation.width, allocation.height,
                              &states, serial);
  wl_array_release(&states);
}

static void
wakefield_compositor_size_allocate (GtkWidget *widget,
                                    GtkAllocation *allocation)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct wl_resource *xdg_surface_resource, *output;

  gtk_widget_set_allocation (widget, allocation);

  if (gtk_widget_get_realized (widget))
    gdk_window_move_resize (priv->event_window,
                            allocation->x,
                            allocation->y,
                            allocation->width,
                            allocation->height);

  wl_resource_for_each (output, &priv->output.resource_list)
    {
      refresh_output (compositor, output);
    }

  wl_resource_for_each (xdg_surface_resource, &priv->xdg_surfaces)
    {
      send_xdg_configure_request (compositor, xdg_surface_resource);
    }
}

static void
wakefield_compositor_state_flags_changed (GtkWidget        *widget,
                                          GtkStateFlags     old_state)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct wl_resource *xdg_surface_resource;

  GTK_WIDGET_CLASS (wakefield_compositor_parent_class)->state_flags_changed (widget, old_state);

  wl_resource_for_each (xdg_surface_resource, &priv->xdg_surfaces)
    {
      send_xdg_configure_request (compositor, xdg_surface_resource);
    }
}

static gboolean
wakefield_compositor_draw (GtkWidget *widget,
                           cairo_t   *cr)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct wl_resource *xdg_surface_resource;

  wl_resource_for_each (xdg_surface_resource, &priv->xdg_surfaces)
    {
      struct wl_resource *surface_resource = wakefield_xdg_surface_get_surface (xdg_surface_resource);
      if (surface_resource)
        wakefield_surface_draw (surface_resource, cr);
    }

  return TRUE;
}

static struct wl_resource *
wakefield_compositor_get_pointer_for_client (WakefieldCompositor *compositor,
                                             struct wl_client *client)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);

  return wl_resource_find_for_client (&priv->seat.pointer.resource_list, client);
}

static struct wl_resource *
wakefield_compositor_get_keyboard_for_client (WakefieldCompositor *compositor,
                                              struct wl_client *client)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);

  return wl_resource_find_for_client (&priv->seat.keyboard.resource_list, client);
}

static void
send_enter (WakefieldCompositor *compositor, struct wl_resource  *surface, double x, double y)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct WakefieldPointer *pointer = &priv->seat.pointer;
  struct wl_resource *pointer_resource;

  pointer_resource = wakefield_compositor_get_pointer_for_client (compositor,
                                                                  wl_resource_get_client (surface));

  pointer->serial = wl_display_next_serial (priv->wl_display);
  if (pointer_resource)
    wl_pointer_send_enter (pointer_resource, pointer->serial,
                           surface,
                           wl_fixed_from_double (x),
                           wl_fixed_from_double (y));
}

static void
send_leave (WakefieldCompositor *compositor, struct wl_resource  *surface)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct WakefieldPointer *pointer = &priv->seat.pointer;
  struct wl_resource *pointer_resource;

  pointer_resource = wakefield_compositor_get_pointer_for_client (compositor,
                                                                  wl_resource_get_client (surface));

  pointer->serial = wl_display_next_serial (priv->wl_display);
  if (pointer_resource)
    wl_pointer_send_leave (pointer_resource, pointer->serial, surface);

  if (pointer->cursor_surface)
    unset_cursor_surface (pointer, pointer->cursor_surface);
}

static uint32_t
convert_gdk_button_to_libinput (int gdk_button)
{
 switch (gdk_button)
   {
   case 3:
     return 273;
   case 2:
     return 274;
   default:
     return gdk_button + 271;
   }
}

static void
wakefield_compositor_clear_grab (WakefieldCompositor *compositor)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct WakefieldPointer *pointer = &priv->seat.pointer;

  if (pointer->grab_popup_surface)
    gdk_device_ungrab (pointer->grab_device, GDK_CURRENT_TIME);
  else
    {
      /* During a passive grab we may have not sent a leave event, send it now */
      if (pointer->current_gdk_surface != pointer->current_surface)
        {
          g_assert (pointer->current_gdk_surface == NULL);
          send_leave (compositor, pointer->current_surface);
          pointer->current_surface = NULL;
        }
    }

  pointer->grab_button = 0;
  pointer->grab_client = NULL;
  pointer->grab_popup_surface = NULL;

  /* We leave grab_device/window/serial around as we need these
     to grant new popups, only zero when initial_surface is unmapped */
}

static gboolean
should_send_pointer_event (WakefieldCompositor *compositor)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct WakefieldPointer *pointer = &priv->seat.pointer;

  return pointer->current_surface != NULL;
}

static void
ensure_surface_entered (WakefieldCompositor *compositor,
                        struct wl_resource  *surface,
                        double x, double y)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct WakefieldPointer *pointer = &priv->seat.pointer;

  if (pointer->current_surface != surface)
    {
      if (pointer->current_surface != NULL)
        send_leave (compositor, pointer->current_surface);

      pointer->current_surface = surface;

      send_enter (compositor, surface, x, y);
    }
}

void
wakefield_compositor_send_button (WakefieldCompositor *compositor,
                                  struct wl_resource *surface,
                                  GdkEventButton *event)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct WakefieldPointer *pointer = &priv->seat.pointer;
  struct wl_resource *pointer_resource, *xdg_popup_resource;
  guint32 button;

  if (event->type == GDK_2BUTTON_PRESS || event->type == GDK_3BUTTON_PRESS)
    return;

  button = convert_gdk_button_to_libinput (event->button);

  if (event->type == GDK_BUTTON_PRESS)
    {
      if (pointer->button_count == 0 && pointer->grab_popup_surface == NULL)
        {
          if (wakefield_surface_get_xdg_surface (surface) != NULL)
            {
              if (!gtk_widget_has_focus (GTK_WIDGET (compositor)))
                gtk_widget_grab_focus (GTK_WIDGET (compositor));
            }
          pointer->grab_button = button;
          pointer->grab_device = gdk_event_get_device ((GdkEvent *)event);
          pointer->grab_window = event->window;
          pointer->grab_initial_surface = surface;
          pointer->grab_client = wl_resource_get_client (surface);
          pointer->grab_time = event->time;
        }
      pointer->button_count++;
    }
  else
    {
      pointer->button_count--;
    }

  if (surface != NULL)
    {
      ensure_surface_entered (compositor, surface, event->x, event->y);
      pointer_resource = wakefield_compositor_get_pointer_for_client (compositor, wl_resource_get_client (surface));
      if (pointer_resource)
        wl_pointer_send_button (pointer_resource, pointer->serial,
                                event->time,
                                button,
                                (event->type == GDK_BUTTON_PRESS ? 1 : 0));
    }

  if (pointer->button_count == 1 && event->type == GDK_BUTTON_PRESS)
    pointer->grab_serial = pointer->serial;

  if (pointer->button_count == 0 && event->type == GDK_BUTTON_RELEASE)
    {
      if (pointer->grab_popup_surface == NULL)
        {
          /* No explicit grab */

          if (pointer->grab_button != 0)
            wakefield_compositor_clear_grab (compositor);
        }
      else
        {
          /* Explicit grab */

          if (priv->seat.pointer.current_surface == NULL)
            {
              wl_resource_for_each (xdg_popup_resource, &priv->xdg_popups)
                wakefield_xdg_popup_close (xdg_popup_resource);
            }
        }
    }
}

void
wakefield_compositor_send_scroll (WakefieldCompositor *compositor,
                                  struct wl_resource *surface,
                                  GdkEventScroll *event)
{
  struct wl_resource *pointer_resource;

  if (surface == NULL)
    return;

  ensure_surface_entered (compositor, surface, event->x, event->y);

  pointer_resource = wakefield_compositor_get_pointer_for_client (compositor,
                                                                  wl_resource_get_client (surface));
  if (pointer_resource && should_send_pointer_event (compositor))
    {
      switch (event->direction)
        {
        case GDK_SCROLL_SMOOTH:
          if (event->delta_x != 0)
            {
              wl_pointer_send_axis (pointer_resource,
                                    event->time,
                                    WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                    wl_fixed_from_double (event->delta_x * 10.0));
            }
          if (event->delta_y != 0)
            {
              wl_pointer_send_axis (pointer_resource,
                                    event->time,
                                    WL_POINTER_AXIS_VERTICAL_SCROLL,
                                    wl_fixed_from_double (event->delta_y * 10.0));
            }
          break;
        case GDK_SCROLL_UP:
          wl_pointer_send_axis (pointer_resource,
                                event->time,
                                WL_POINTER_AXIS_VERTICAL_SCROLL,
                                wl_fixed_from_int (-10.0));
          break;
        case GDK_SCROLL_DOWN:
          wl_pointer_send_axis (pointer_resource,
                                event->time,
                                WL_POINTER_AXIS_VERTICAL_SCROLL,
                                wl_fixed_from_int (10.0));
          break;
        case GDK_SCROLL_LEFT:
          wl_pointer_send_axis (pointer_resource,
                                event->time,
                                WL_POINTER_AXIS_VERTICAL_SCROLL,
                                wl_fixed_from_int (-10.0));
          break;
        case GDK_SCROLL_RIGHT:
          wl_pointer_send_axis (pointer_resource,
                                event->time,
                                WL_POINTER_AXIS_VERTICAL_SCROLL,
                                wl_fixed_from_int (10.0));
          break;
        }
    }
}

void
wakefield_compositor_send_motion (WakefieldCompositor *compositor,
                                  struct wl_resource *surface,
                                  GdkEventMotion *event)
{
  struct wl_resource *pointer_resource;

  if (surface == NULL)
    return;

  ensure_surface_entered (compositor, surface, event->x, event->y);

  pointer_resource = wakefield_compositor_get_pointer_for_client (compositor,
                                                                  wl_resource_get_client (surface));
  if (pointer_resource && should_send_pointer_event (compositor))
    {
      wl_pointer_send_motion (pointer_resource,
                              event->time,
                              wl_fixed_from_double (event->x),
                              wl_fixed_from_double (event->y));
    }
}

void
wakefield_compositor_send_enter (WakefieldCompositor *compositor,
                                 struct wl_resource  *surface,
                                 GdkEventCrossing *event)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct WakefieldPointer *pointer = &priv->seat.pointer;
  gboolean has_implicit_grab;

  if (surface == NULL)
    return;

  pointer->current_gdk_surface = surface;

  /* During implicit grabs we get enter/leave to the grabbed window, but these
     don't exist in wayland, so never send them */
  has_implicit_grab = pointer->grab_button != 0 && pointer->grab_popup_surface == NULL;
  if (has_implicit_grab)
    return;

  /* We may have ignored a leave event due to an implicit grab, so we need
     to send it now before sending an enter to some other surface */
  ensure_surface_entered (compositor, surface, event->x, event->y);
}

void
wakefield_compositor_send_leave (WakefieldCompositor *compositor,
                                 struct wl_resource  *surface,
                                 GdkEventCrossing *event)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct WakefieldPointer *pointer = &priv->seat.pointer;
  gboolean has_implicit_grab;

  if (surface == NULL)
    return;

  pointer->current_gdk_surface = NULL;

  /* During implicit grabs we get enter/leave to the grabbed window, but these
     don't exist in wayland, so never send them */
  has_implicit_grab = pointer->grab_button != 0 && pointer->grab_popup_surface == NULL;
  if (has_implicit_grab)
    return;

  /* We may have left this surface already when/if it was unmapped */
  if (pointer->current_surface == NULL)
    return;

  g_assert (pointer->current_surface == surface);
  pointer->current_surface = NULL;

  send_leave (compositor, surface);
}

static void
wakefield_compositor_send_keyboard_enter (WakefieldCompositor *compositor,
                                          struct wl_resource *surface)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct WakefieldKeyboard *keyboard = &priv->seat.keyboard;
  struct wl_resource *keyboard_resource;
  struct wl_array keys;

  g_assert (keyboard->focus == NULL);
  keyboard->focus = surface;

  wl_array_init (&keys);

  keyboard_resource = wakefield_compositor_get_keyboard_for_client (compositor,
                                                                   wl_resource_get_client (surface));
  if (keyboard_resource)
    {
      wl_keyboard_send_modifiers (keyboard_resource,
                                  wl_display_next_serial (priv->wl_display),
                                  keyboard->mods_depressed,
                                  keyboard->mods_latched,
                                  keyboard->mods_locked,
                                  keyboard->group);
      wl_keyboard_send_enter (keyboard_resource, wl_display_next_serial (priv->wl_display), surface, &keys);
    }

  wl_array_release (&keys);
}

static void
wakefield_compositor_send_keyboard_leave (WakefieldCompositor *compositor,
                                          struct wl_resource *surface)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct WakefieldKeyboard *keyboard = &priv->seat.keyboard;
  struct wl_resource *keyboard_resource;
  uint32_t serial = wl_display_next_serial (priv->wl_display);

  g_assert (keyboard->focus == surface);
  keyboard->focus = NULL;

  keyboard_resource = wakefield_compositor_get_keyboard_for_client (compositor,
                                                                    wl_resource_get_client (surface));
  if (keyboard_resource)
    wl_keyboard_send_leave (keyboard_resource, serial, surface);
}



static struct wl_resource *
wakefield_compositor_get_xdg_surface_for_window (WakefieldCompositor *compositor,
                                                 GdkWindow *window)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct wl_resource *xdg_surface_resource;

  wl_resource_for_each (xdg_surface_resource, &priv->xdg_surfaces)
    {
      if (window == wakefield_xdg_surface_get_window (xdg_surface_resource))
        return wakefield_xdg_surface_get_surface (xdg_surface_resource);
    }

  return NULL;
}

static struct wl_resource *
wakefield_compositor_get_topmost_surface (WakefieldCompositor *compositor)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct wl_resource *xdg_surface_resource;

  wl_resource_for_each_reverse (xdg_surface_resource, &priv->xdg_surfaces)
    {
      struct wl_resource *surface_resource;
      surface_resource = wakefield_xdg_surface_get_surface (xdg_surface_resource);
      if (surface_resource != NULL && wakefield_surface_is_mapped (surface_resource))
        return surface_resource;
    }

  return NULL;
}

static gboolean
wakefield_compositor_button_press_event (GtkWidget      *widget,
                                         GdkEventButton *event)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  WakefieldCompositorPrivate *priv =
    wakefield_compositor_get_instance_private (compositor);
  struct WakefieldPointer *pointer = &priv->seat.pointer;
  struct wl_resource *surface;

  pointer->serial = wl_display_next_serial (priv->wl_display);

  surface = wakefield_compositor_get_xdg_surface_for_window (compositor, event->window);

  if (surface)
    wakefield_compositor_send_button (compositor, surface, event);

  return TRUE;
}

static gboolean
wakefield_compositor_button_release_event (GtkWidget      *widget,
                                           GdkEventButton *event)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  WakefieldCompositorPrivate *priv =
    wakefield_compositor_get_instance_private (compositor);
  struct WakefieldPointer *pointer = &priv->seat.pointer;
  struct wl_resource *surface;

  pointer->serial = wl_display_next_serial (priv->wl_display);

  surface = wakefield_compositor_get_xdg_surface_for_window (compositor, event->window);

  if (surface)
    wakefield_compositor_send_button (compositor, surface, event);

  return TRUE;
}

static gboolean
wakefield_compositor_scroll_event (GtkWidget      *widget,
                                   GdkEventScroll *event)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  struct wl_resource *surface;

  surface = wakefield_compositor_get_xdg_surface_for_window (compositor, event->window);

  if (surface)
    wakefield_compositor_send_scroll (compositor, surface, event);

  return TRUE;
}

static gboolean
wakefield_compositor_motion_notify_event (GtkWidget      *widget,
                                          GdkEventMotion *event)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  struct wl_resource *surface;

  surface = wakefield_compositor_get_xdg_surface_for_window (compositor, event->window);

  if (surface)
    wakefield_compositor_send_motion (compositor, surface, event);

  return FALSE;
}

static gboolean
wakefield_compositor_enter_notify_event (GtkWidget        *widget,
                                         GdkEventCrossing *event)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  struct wl_resource *surface;

  surface = wakefield_compositor_get_xdg_surface_for_window (compositor, event->window);
  if (event->mode == GDK_CROSSING_NORMAL && surface)
    wakefield_compositor_send_enter (compositor,
                                     surface,
                                     event);

  return FALSE;
}

static gboolean
wakefield_compositor_leave_notify_event (GtkWidget        *widget,
                                         GdkEventCrossing *event)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  struct wl_resource *surface;

  surface = wakefield_compositor_get_xdg_surface_for_window (compositor, event->window);

  if (event->mode == GDK_CROSSING_NORMAL && surface)
    wakefield_compositor_send_leave (compositor, surface, event);

  return FALSE;
}

static gboolean
wakefield_compositor_focus_in_event (GtkWidget     *widget,
                                     GdkEventFocus *event)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  struct wl_resource *surface;

  surface = wakefield_compositor_get_topmost_surface (compositor);

  if (surface)
    wakefield_compositor_send_keyboard_enter (compositor, surface);

  return FALSE;
}

static gboolean
wakefield_compositor_focus_out_event (GtkWidget     *widget,
                                      GdkEventFocus *event)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct WakefieldKeyboard *keyboard = &priv->seat.keyboard;


  if (keyboard->focus && wakefield_surface_get_xdg_surface (keyboard->focus))
    wakefield_compositor_send_keyboard_leave (compositor, keyboard->focus);

  return FALSE;
}

static void
update_modifier_state (WakefieldCompositor *compositor, struct wl_resource *keyboard_resource, GdkEventKey *event)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct WakefieldKeyboard *keyboard = &priv->seat.keyboard;
  guint32 old_mods_depressed = keyboard->mods_depressed;
  guint32 old_mods_latched = keyboard->mods_latched;
  guint32 old_mods_locked = keyboard->mods_locked;
  guint32 old_group = keyboard->group;

  keyboard->mods_depressed = 0;
  keyboard->mods_latched = 0;
  keyboard->mods_locked = 0;
  keyboard->group = event->group;

  if (event->state & GDK_SHIFT_MASK)
    keyboard->mods_depressed |= 1 << keyboard->shift_mod;
  if (event->state & GDK_LOCK_MASK)
    keyboard->mods_depressed |= 1 << keyboard->caps_mod;
  if (event->state & GDK_CONTROL_MASK)
    keyboard->mods_depressed |= 1 << keyboard->ctrl_mod;
  if (event->state & GDK_MOD1_MASK)
    keyboard->mods_depressed |= 1 << keyboard->alt_mod;
  if (event->state & GDK_MOD2_MASK)
    keyboard->mods_depressed |= 1 << keyboard->mod2_mod;
  if (event->state & GDK_MOD3_MASK)
    keyboard->mods_depressed |= 1 << keyboard->mod3_mod;
  if (event->state & GDK_MOD4_MASK)
    keyboard->mods_depressed |= 1 << keyboard->super_mod;
  if (event->state & GDK_MOD5_MASK)
    keyboard->mods_depressed |= 1 << keyboard->mod5_mod;


  if (old_mods_depressed != keyboard->mods_depressed ||
      old_mods_latched != keyboard->mods_latched ||
      old_mods_locked != keyboard->mods_locked ||
      old_group != keyboard->group)
    wl_keyboard_send_modifiers (keyboard_resource,
                                wl_display_next_serial (priv->wl_display),
                                keyboard->mods_depressed,
                                keyboard->mods_latched,
                                keyboard->mods_locked,
                                keyboard->group);
}

static gboolean
wakefield_compositor_key_press_event (GtkWidget *widget,
                                      GdkEventKey *event)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct WakefieldKeyboard *keyboard = &priv->seat.keyboard;
  struct wl_resource *keyboard_resource;
  uint32_t serial = wl_display_next_serial (priv->wl_display);

  if (keyboard->focus != NULL)
    {
      keyboard_resource = wakefield_compositor_get_keyboard_for_client (compositor,
                                                                        wl_resource_get_client (keyboard->focus));

      update_modifier_state (compositor, keyboard_resource, event);
      wl_keyboard_send_key (keyboard_resource, serial, event->time, event->hardware_keycode - 8, WL_KEYBOARD_KEY_STATE_PRESSED);

    }

  return FALSE;
}

static gboolean
wakefield_compositor_key_release_event (GtkWidget     *widget,
                                        GdkEventKey *event)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct WakefieldKeyboard *keyboard = &priv->seat.keyboard;
  struct wl_resource *keyboard_resource;
  uint32_t serial = wl_display_next_serial (priv->wl_display);

  if (keyboard->focus != NULL)
    {
      keyboard_resource = wakefield_compositor_get_keyboard_for_client (compositor,
                                                                        wl_resource_get_client (keyboard->focus));

      update_modifier_state (compositor, keyboard_resource, event);
      wl_keyboard_send_key (keyboard_resource, serial, event->time, event->hardware_keycode - 8, WL_KEYBOARD_KEY_STATE_RELEASED);
    }

  return FALSE;
}

static void
resource_release (struct wl_client *client,
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
unset_cursor_surface (struct WakefieldPointer *pointer,
                      WakefieldSurface *cursor_surface)
{
  g_signal_handler_disconnect (cursor_surface,
                               pointer->cursor_surface_commit_listener);
  g_object_remove_weak_pointer (G_OBJECT (cursor_surface),
                                (gpointer*)&pointer->cursor_surface);
  pointer->cursor_surface = NULL;
}

static void
pointer_cursor_surface_committed (WakefieldSurface *surface,
                                  GdkWindow *window)
{
  WakefieldCompositor *compositor = wakefield_surface_get_compositor (surface);
  WakefieldCompositorPrivate *priv =
    wakefield_compositor_get_instance_private (compositor);
  struct WakefieldPointer *pointer = &priv->seat.pointer;
  cairo_surface_t *cursor_surface;
  int w, h;

  cursor_surface = wakefield_surface_create_cairo_surface (surface, &w, &h);
  if (cursor_surface)
    {
      GdkCursor *gdk_cursor;

      /* Note: XRender BadMatches if the hotspot is outside the cursor, so
         limit it here */
      gdk_cursor = gdk_cursor_new_from_surface (gdk_window_get_display (window),
                                                cursor_surface,
                                                MIN (w, pointer->hot_x),
                                                MIN (h, pointer->hot_y));
      cairo_surface_destroy (cursor_surface);
      gdk_window_set_cursor (window, gdk_cursor);
      g_object_unref (gdk_cursor);
    }
}

static void
pointer_set_cursor (struct wl_client *client,
                    struct wl_resource *resource,
                    uint32_t serial,
                    struct wl_resource *surface_resource,
                    int32_t x, int32_t y)
{
  struct WakefieldPointer *pointer = wl_resource_get_user_data (resource);
  WakefieldSurface *cursor_surface = NULL;
  GdkWindow *window;

  if (surface_resource)
    {
      cursor_surface = wl_resource_get_user_data (surface_resource);

      switch (wakefield_surface_get_role (surface_resource))
        {
        case WAKEFIELD_SURFACE_ROLE_NONE:
        case WAKEFIELD_SURFACE_ROLE_POINTER_CURSOR:
          break;
        case WAKEFIELD_SURFACE_ROLE_XDG_SURFACE:
        case WAKEFIELD_SURFACE_ROLE_XDG_POPUP:
          wl_resource_post_error (resource, WL_POINTER_ERROR_ROLE,
                                  "This wl_surface already has a role");
          break;
        }

      wakefield_surface_set_role (surface_resource,
                                  WAKEFIELD_SURFACE_ROLE_POINTER_CURSOR);
    }

  if (pointer->serial != serial)
    return;

  if (pointer->current_surface == NULL)
    return;

  if (pointer->cursor_surface == cursor_surface)
    return;

  if (pointer->cursor_surface)
    unset_cursor_surface (pointer, pointer->cursor_surface);

  if (cursor_surface)
    {
      window = wakefield_surface_get_window (pointer->current_surface);
      pointer->hot_x = x;
      pointer->hot_y = y;
      pointer->cursor_surface_commit_listener =
        g_signal_connect_object (cursor_surface, "committed",
                                 G_CALLBACK (pointer_cursor_surface_committed),
                                 window, 0);
      pointer->cursor_surface = cursor_surface;
      g_object_add_weak_pointer (G_OBJECT (cursor_surface),
                                 (gpointer*)&pointer->cursor_surface);
      pointer_cursor_surface_committed (cursor_surface, window);
    }
  else
    pointer->cursor_surface = NULL;;
}

static const struct wl_pointer_interface pointer_implementation = {
  pointer_set_cursor,
  resource_release,
};

static void
seat_get_pointer (struct wl_client    *client,
                  struct wl_resource  *seat_resource,
                  uint32_t             id)
{
  struct WakefieldSeat *seat = wl_resource_get_user_data (seat_resource);
  struct WakefieldPointer *pointer = &seat->pointer;
  struct wl_resource *cr;

  cr = wl_resource_create (client, &wl_pointer_interface, wl_resource_get_version (seat_resource), id);
  wl_resource_set_implementation (cr, &pointer_implementation, pointer, unbind_resource);
  wl_list_insert (&pointer->resource_list, wl_resource_get_link (cr));
}

static void
wakefield_pointer_init (struct WakefieldPointer *pointer)
{
  wl_list_init (&pointer->resource_list);
  pointer->cursor_surface = NULL;
}

static const struct wl_keyboard_interface keyboard_implementation = {
  resource_release,
};

static void
seat_get_keyboard (struct wl_client    *client,
                   struct wl_resource  *seat_resource,
                   uint32_t             id)
{
  struct WakefieldSeat *seat = wl_resource_get_user_data (seat_resource);
  struct WakefieldKeyboard *keyboard = &seat->keyboard;
  struct wl_resource *cr;

  cr = wl_resource_create (client, &wl_keyboard_interface, wl_resource_get_version (seat_resource), id);
  wl_resource_set_implementation (cr, &keyboard_implementation, keyboard, unbind_resource);
  wl_list_insert (&keyboard->resource_list, wl_resource_get_link (cr));

  if (keyboard->keymap_fd != -1)
    {
      /* FIXME: Read repeat info from gsettings */
      wl_keyboard_send_repeat_info (cr, 0, 0);
      wl_keyboard_send_keymap (cr,
                               WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                               keyboard->keymap_fd,
                               keyboard->keymap_size);
    }
}

static struct xkb_keymap *
get_keymap (WakefieldCompositor *compositor,
            struct WakefieldKeyboard *keyboard)
{
  GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (compositor));

#if defined(GDK_WINDOWING_X11)
  if (GDK_IS_X11_DISPLAY (display) && keyboard->has_x11_xkb)
    {
      Display *xdisplay = gdk_x11_display_get_xdisplay (display);
      xcb_connection_t *conn = XGetXCBConnection (xdisplay);
      int32_t core_id = xkb_x11_get_core_keyboard_device_id (conn);

      return xkb_x11_keymap_new_from_device (keyboard->context,
                                             conn,
                                             core_id,
                                             XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
#endif

  return NULL;
}

static int
create_anonymous_file (gsize size)
{
  char *tmpname;
  int fd;
  int ret;

  tmpname = g_build_filename (g_get_user_runtime_dir (), "/weston-shared-XXXXXX", NULL);
  fd = g_mkstemp (tmpname);
  unlink (tmpname);
  free (tmpname);

  if (fd < 0)
    return -1;

#ifdef HAVE_POSIX_FALLOCATE
  ret = posix_fallocate (fd, 0, size);
  if (ret != 0)
    {
      close (fd);
      errno = ret;
      return -1;
    }
#else
  ret = ftruncate (fd, size);
  if (ret < 0)
    {
      close (fd);
      return -1;
    }
#endif

  return fd;
}

static void
write_all (int           fd,
           const char*    buf,
           gsize         len)
{
  while (len > 0)
    {
      gssize bytes_written = write (fd, buf, len);
      if (bytes_written < 0)
        g_error ("Failed to write to fd %d: %s",
                 fd, strerror (errno));
      buf += bytes_written;
      len -= bytes_written;
    }
}

static void
update_keymap (WakefieldCompositor *compositor,
               struct WakefieldKeyboard *keyboard)
{
  struct wl_resource *keyboard_resource;
  struct xkb_keymap *keymap;
  char *str;
  gsize size;

  if (keyboard->keymap)
    xkb_keymap_unref (keyboard->keymap);
  keyboard->keymap = NULL;
  if (keyboard->keymap_fd != -1)
    close (keyboard->keymap_fd);
  keyboard->keymap_fd = -1;
  keyboard->keymap_size = 0;

  keymap = get_keymap (compositor, keyboard);
  if (keymap)
    {
      str = xkb_keymap_get_as_string (keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
      if (str)
        {
          size = strlen (str);
          keyboard->keymap_fd = create_anonymous_file (size);
          if (keyboard->keymap_fd != -1)
            {
              write_all (keyboard->keymap_fd, str, size);
              keyboard->keymap = xkb_keymap_ref (keymap);
              keyboard->keymap_size = size;
            }
          free (str);
        }

      keyboard->shift_mod = xkb_keymap_mod_get_index (keymap, XKB_MOD_NAME_SHIFT);
      keyboard->caps_mod = xkb_keymap_mod_get_index (keymap, XKB_MOD_NAME_CAPS);
      keyboard->ctrl_mod = xkb_keymap_mod_get_index (keymap, XKB_MOD_NAME_CTRL);
      keyboard->alt_mod = xkb_keymap_mod_get_index (keymap, XKB_MOD_NAME_ALT);
      keyboard->mod2_mod = xkb_keymap_mod_get_index (keymap, "Mod2");
      keyboard->mod3_mod = xkb_keymap_mod_get_index (keymap, "Mod3");
      keyboard->super_mod = xkb_keymap_mod_get_index (keymap, XKB_MOD_NAME_LOGO);
      keyboard->mod5_mod = xkb_keymap_mod_get_index (keymap, "Mod5");

      keyboard->num_led = xkb_keymap_led_get_index (keymap, XKB_LED_NAME_NUM);
      keyboard->caps_led = xkb_keymap_led_get_index (keymap, XKB_LED_NAME_CAPS);
      keyboard->scroll_led = xkb_keymap_led_get_index (keymap, XKB_LED_NAME_SCROLL);

      xkb_keymap_unref (keymap);
    }

  if (keyboard->keymap_fd != -1)
    {
      wl_resource_for_each (keyboard_resource, &keyboard->resource_list)
        wl_keyboard_send_keymap (keyboard_resource,
                                 WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                 keyboard->keymap_fd,
                                 keyboard->keymap_size);
    }
}

static void
wakefield_keyboard_init (WakefieldCompositor *compositor,
                         struct WakefieldKeyboard *keyboard)
{
  GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (compositor));

  wl_list_init (&keyboard->resource_list);

  keyboard->context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);

#if defined(GDK_WINDOWING_X11)
  if (GDK_IS_X11_DISPLAY (display))
    {
      Display *xdisplay = gdk_x11_display_get_xdisplay (display);
      xcb_connection_t *conn = XGetXCBConnection (xdisplay);

      keyboard->has_x11_xkb =
        xkb_x11_setup_xkb_extension (conn,
                                     XKB_X11_MIN_MAJOR_XKB_VERSION, XKB_X11_MIN_MINOR_XKB_VERSION,
                                     0,
                                     NULL, NULL, NULL, NULL);
    }
#endif

  keyboard->keymap_fd = -1;
  update_keymap (compositor, keyboard);
}

#define SEAT_VERSION 4

static const struct wl_seat_interface seat_interface = {
  seat_get_pointer,
  seat_get_keyboard, /* get_keyboard */
  .get_touch = NULL,
  .release = NULL,
};

static void
wl_seat_destructor (struct wl_resource *resource)
{
}

static void
bind_seat (struct wl_client *client,
           void *data,
           uint32_t version,
           uint32_t id)
{
  struct WaylandSeat *seat = data;
  struct wl_resource *cr;

  cr = wl_resource_create (client, &wl_seat_interface, version, id);
  wl_resource_set_implementation (cr, &seat_interface, seat, wl_seat_destructor);
  wl_seat_send_capabilities (cr, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
  wl_seat_send_name (cr, "seat0");
}

static void
wakefield_seat_init (WakefieldCompositor *compositor,
                     struct WakefieldSeat *seat,
                     struct wl_display    *wl_display)
{
  wakefield_pointer_init (&seat->pointer);
  wakefield_keyboard_init (compositor, &seat->keyboard);

  wl_global_create (wl_display, &wl_seat_interface, SEAT_VERSION, seat, bind_seat);
}

static void
bind_output (struct wl_client *client,
             void *data,
             uint32_t version,
             uint32_t id)
{
  WakefieldCompositor *compositor = data;
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct WakefieldOutput *output = &priv->output;
  struct wl_resource *cr;

  cr = wl_resource_create (client, &wl_output_interface, version, id);
  wl_resource_set_implementation (cr, NULL, output, unbind_resource);
  wl_list_insert (&output->resource_list, wl_resource_get_link (cr));

  wl_output_send_geometry (cr,
                           0, 0,
                           0, 0, /* mm */
                           0, /* subpixel */
                           "Wakefield", "Gtk",
                           WL_OUTPUT_TRANSFORM_NORMAL);

  refresh_output (compositor, cr);
}

#define WL_OUTPUT_VERSION 2

static void
wakefield_output_init (WakefieldCompositor *compositor)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  wl_list_init (&priv->output.resource_list);
  wl_global_create (priv->wl_display, &wl_output_interface,
                    WL_OUTPUT_VERSION, compositor, bind_output);
}

static GSource * wayland_event_source_new (struct wl_display *display);

cairo_region_t *
wakefield_region_get_region (struct wl_resource *region_resource)
{
  struct WakefieldRegion *region = wl_resource_get_user_data (region_resource);
  return cairo_region_copy (region->region);
}

static void
wl_region_add (struct wl_client *client,
               struct wl_resource *resource,
               gint32 x,
               gint32 y,
               gint32 width,
               gint32 height)
{
  struct WakefieldRegion *region = wl_resource_get_user_data (resource);
  cairo_rectangle_int_t rectangle = { x, y, width, height };
  cairo_region_union_rectangle (region->region, &rectangle);
}

static void
wl_region_subtract (struct wl_client *client,
                    struct wl_resource *resource,
                    gint32 x,
                    gint32 y,
                    gint32 width,
                    gint32 height)
{
  struct WakefieldRegion *region = wl_resource_get_user_data (resource);
  cairo_rectangle_int_t rectangle = { x, y, width, height };
  cairo_region_subtract_rectangle (region->region, &rectangle);
}

static const struct wl_region_interface region_interface = {
  resource_release,
  wl_region_add,
  wl_region_subtract
};

static void
wl_region_destructor (struct wl_resource *resource)
{
  struct WakefieldRegion *region = wl_resource_get_user_data (resource);

  cairo_region_destroy (region->region);
  g_slice_free (struct WakefieldRegion, region);
}

static void
wl_compositor_create_region (struct wl_client *client,
                             struct wl_resource *compositor_resource,
                             uint32_t id)
{
  struct WakefieldRegion *region = g_slice_new0 (struct WakefieldRegion);

  region->resource = wl_resource_create (client, &wl_region_interface, wl_resource_get_version (compositor_resource), id);
  wl_resource_set_implementation (region->resource, &region_interface, region, wl_region_destructor);

  region->region = cairo_region_create ();
}

void
wakefield_compositor_surface_unmapped (WakefieldCompositor *compositor,
                                       struct wl_resource  *surface)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct WakefieldPointer *pointer = &priv->seat.pointer;
  struct WakefieldKeyboard *keyboard = &priv->seat.keyboard;
  struct wl_resource *xdg_surface = wakefield_surface_get_xdg_surface (surface);

  if (keyboard->focus == surface)
    {
      wakefield_compositor_send_keyboard_leave (compositor, surface);

      if (gtk_widget_has_focus (GTK_WIDGET (compositor)))
        {
          struct wl_resource *topmost_surface = wakefield_compositor_get_topmost_surface (compositor);
          if (topmost_surface)
            wakefield_compositor_send_keyboard_enter (compositor, topmost_surface);
        }
    }

  if (pointer->grab_popup_surface == surface)
    wakefield_compositor_clear_grab (compositor);

  if (pointer->grab_initial_surface == surface)
    {
      if (pointer->grab_popup_surface == NULL &&
          pointer->grab_button != 0)
        wakefield_compositor_clear_grab (compositor);

      pointer->grab_serial = 0;
      pointer->grab_device = NULL;
      pointer->grab_window = NULL;
      pointer->grab_initial_surface = NULL;

    }

  if (pointer->current_surface == surface)
    {
      send_leave (compositor, surface);
      pointer->current_surface = NULL;
    }

  if (xdg_surface)
    gtk_widget_queue_draw (GTK_WIDGET (compositor));

}

void
wakefield_compositor_surface_mapped (WakefieldCompositor *compositor,
                                     struct wl_resource  *surface)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct wl_resource *xdg_surface = wakefield_surface_get_xdg_surface  (surface);
  struct WakefieldKeyboard *keyboard = &priv->seat.keyboard;

  if (xdg_surface && gtk_widget_get_realized (GTK_WIDGET (compositor)))
    {
      if (gtk_widget_has_focus (GTK_WIDGET (compositor)) &&
          keyboard->focus == NULL)
        wakefield_compositor_send_keyboard_enter (compositor, surface);

      wakefield_xdg_surface_realize (xdg_surface, priv->event_window);
    }
}

static void
wl_compositor_create_surface (struct wl_client *client,
                              struct wl_resource *compositor_resource,
                              uint32_t id)
{
  WakefieldCompositor *compositor = wl_resource_get_user_data (compositor_resource);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct wl_resource *surface;

  surface = wakefield_surface_new (compositor, client, compositor_resource, id);
  wl_list_insert (&priv->surfaces, wl_resource_get_link (surface));
}


static const struct wl_compositor_interface compositor_interface = {
  wl_compositor_create_surface,
  wl_compositor_create_region
};


static void
xdg_shell_destroy (struct wl_client *client,
                   struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
xdg_use_unstable_version (struct wl_client *client,
                          struct wl_resource *resource,
                          int32_t version)
{
  if (version > 5)
    {
      wl_resource_post_error (resource, 1,
                              "xdg-shell:: version not implemented yet.");
      return;
    }
}

static void
xdg_get_xdg_surface (struct wl_client *client,
                     struct wl_resource *shell_resource,
                     uint32_t id,
                     struct wl_resource *surface_resource)
{
  struct wl_resource *xdg_surface, *output_resource;
  WakefieldCompositor *compositor = wl_resource_get_user_data (shell_resource);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  WakefieldSurfaceRole role;

  if (surface_resource == NULL)
    {
      wl_resource_post_error (shell_resource,
                              WL_DISPLAY_ERROR_INVALID_OBJECT,
                              "xdg_shell::get_xdg_popup requires a surface");
      return;
    }

  role = wakefield_surface_get_role (surface_resource);
  if (role)
    {
      wl_resource_post_error (shell_resource, XDG_SHELL_ERROR_ROLE,
                              "This wl_surface already has a role");
      return;
    }

  xdg_surface = wakefield_xdg_surface_new (client, shell_resource, id, surface_resource);
  wl_list_insert (priv->xdg_surfaces.prev, wl_resource_get_link (xdg_surface));

  output_resource = wl_resource_find_for_client (&priv->output.resource_list, client);
  wl_surface_send_enter (surface_resource, output_resource);

  send_xdg_configure_request (compositor, xdg_surface);
}

static void
xdg_get_xdg_popup (struct wl_client *client,
                   struct wl_resource *shell_resource,
                   uint32_t id,
                   struct wl_resource *surface_resource,
                   struct wl_resource *parent_resource,
                   struct wl_resource *seat_resource,
                   uint32_t serial,
                   int32_t x, int32_t y)
{
  WakefieldCompositor *compositor = wl_resource_get_user_data (shell_resource);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct WakefieldPointer *pointer = &priv->seat.pointer;
  struct wl_resource *xdg_popup, *output_resource;
  WakefieldSurfaceRole role, parent_role;

  if (surface_resource == NULL)
    {
      wl_resource_post_error (shell_resource,
                              WL_DISPLAY_ERROR_INVALID_OBJECT,
                              "xdg_shell::get_xdg_popup requires a surface");
      return;
    }

  role = wakefield_surface_get_role (surface_resource);
  if (role)
    {
      wl_resource_post_error (shell_resource, XDG_SHELL_ERROR_ROLE,
                              "This wl_surface already has a role");
      return;
    }

  if (parent_resource == NULL)
    {
      wl_resource_post_error (shell_resource,
                              WL_DISPLAY_ERROR_INVALID_OBJECT,
                              "xdg_shell::get_xdg_popup requires a parent shell surface");
      return;
    }

  parent_role = wakefield_surface_get_role (parent_resource);

  if (parent_role != WAKEFIELD_SURFACE_ROLE_XDG_SURFACE &&
      parent_role != WAKEFIELD_SURFACE_ROLE_XDG_POPUP)
    {
      wl_resource_post_error (shell_resource,
                              XDG_POPUP_ERROR_INVALID_PARENT,
                              "xdg_popup parent was invalid");
      return;
    }

  if (!wl_list_empty (&priv->xdg_popups))
    {
      struct wl_resource *top_xdg_popup = wl_resource_from_link (priv->xdg_popups.next);
      if (parent_resource != wakefield_xdg_surface_get_surface (top_xdg_popup))
        {
          wl_resource_post_error (shell_resource,
                                  XDG_POPUP_ERROR_NOT_THE_TOPMOST_POPUP,
                                  "xdg_popup was not created on the "
                                  "topmost popup");
          return;
        }
    }

  xdg_popup = wakefield_xdg_popup_new (compositor, client, shell_resource, id, surface_resource, parent_resource, serial, x, y);
  wl_list_insert (&priv->xdg_popups, wl_resource_get_link (xdg_popup));

  output_resource = wl_resource_find_for_client (&priv->output.resource_list, client);
  wl_surface_send_enter (surface_resource, output_resource);

  if (wakefield_xdg_popup_get_serial (xdg_popup) == pointer->grab_serial)
    {
      pointer->grab_popup_surface = surface_resource;
      gdk_device_grab (pointer->grab_device,
                       wakefield_surface_get_window (parent_resource),
                       GDK_OWNERSHIP_NONE,
                       TRUE,
                       GDK_POINTER_MOTION_MASK |
                       GDK_BUTTON_PRESS_MASK |
                       GDK_BUTTON_RELEASE_MASK |
                       GDK_SCROLL_MASK |
                       GDK_ENTER_NOTIFY_MASK |
                       GDK_LEAVE_NOTIFY_MASK,
                       NULL,
                       pointer->grab_time);
    }
  else
    {
      wakefield_xdg_popup_close (xdg_popup);
    }
}

static void
xdg_pong (struct wl_client *client,
          struct wl_resource *resource,
          uint32_t serial)
{
  wl_resource_post_error (resource, 1,
                          "xdg-shell:: pong not implemented yet.");
}

static const struct xdg_shell_interface xdg_implementation = {
  xdg_shell_destroy,
  xdg_use_unstable_version,
  xdg_get_xdg_surface,
  xdg_get_xdg_popup,
  xdg_pong
};

#define XDG_SHELL_VERSION 1

static void
bind_xdg_shell(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
  WakefieldCompositor *compositor = data;
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct wl_resource *cr;

  cr = wl_resource_create (client,  &xdg_shell_interface, XDG_SHELL_VERSION, id);
  wl_resource_set_implementation (cr, &xdg_implementation, compositor, unbind_resource);
  wl_list_insert (&priv->shell_resources, wl_resource_get_link (cr));
}

static void
bind_compositor (struct wl_client *client,
                 void *data,
                 uint32_t version,
                 uint32_t id)
{
  WakefieldCompositor *compositor = data;
  struct wl_resource *cr;

  cr = wl_resource_create (client, &wl_compositor_interface, version, id);
  wl_resource_set_implementation (cr, &compositor_interface, compositor, NULL);
}

#define WL_COMPOSITOR_VERSION 3

struct wl_display *
wakefield_compositor_get_display (WakefieldCompositor *compositor)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);

  return priv->wl_display;
}

static void
wakefield_compositor_init (WakefieldCompositor *compositor)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);

  gtk_widget_set_has_window (GTK_WIDGET (compositor), FALSE);
  gtk_widget_set_can_focus (GTK_WIDGET (compositor), TRUE);

  priv->wl_display = wl_display_create ();
  wl_display_init_shm (priv->wl_display);

  wl_global_create (priv->wl_display, &wl_compositor_interface,
                    WL_COMPOSITOR_VERSION, compositor, bind_compositor);

  wl_global_create (priv->wl_display, &xdg_shell_interface,
                    XDG_SHELL_VERSION, compositor, bind_xdg_shell);
  wl_list_init (&priv->shell_resources);

  priv->data_device = wakefield_data_device_new (compositor);

  wakefield_seat_init (compositor, &priv->seat, priv->wl_display);
  wakefield_output_init (compositor);

  wl_list_init (&priv->surfaces);
  wl_list_init (&priv->xdg_surfaces);
  wl_list_init (&priv->xdg_popups);

  /* Attach the wl_event_loop to ours */
  priv->wayland_source = wayland_event_source_new (priv->wl_display);
  g_source_attach (priv->wayland_source, NULL);
}

WakefieldCompositor *
wakefield_compositor_new (void)
{
  return g_object_new (WAKEFIELD_TYPE_COMPOSITOR, NULL);
}

gboolean
wakefield_compositor_add_socket (WakefieldCompositor *compositor,
                                 const char *name,
                                 GError **error)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);

  if (wl_display_add_socket (priv->wl_display, name) != 0)
    {
      int errsv = errno;

      g_set_error (error,
                   G_IO_ERROR,
                   g_io_error_from_errno (errsv),
                   _("Error adding wayland display '%s' socket: %s"),
                   name ? name : "NULL",
                   strerror (errsv));
      return FALSE;
    }

  return TRUE;
}


const char *
wakefield_compositor_add_socket_auto (WakefieldCompositor *compositor,
                                      GError **error)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  const char *name;

  name = wl_display_add_socket_auto (priv->wl_display);
  if (name == NULL)
    {
      int errsv = errno;

      g_set_error (error,
                   G_IO_ERROR,
                   g_io_error_from_errno (errsv),
                   _("Error adding automatic socket: %s"),
                   strerror (errsv));
      return NULL;
    }

  return name;
}

typedef struct {
  struct wl_listener listener;
  GDestroyNotify destroy_notify;
  gpointer user_data;
} WakefieldClientDestroyListener;

static void
client_destroyed (struct wl_listener *listener, void *data)
{
  WakefieldClientDestroyListener *w_listener = (WakefieldClientDestroyListener *)listener;

  w_listener->destroy_notify (w_listener->user_data);
}

int
wakefield_compositor_create_client_fd (WakefieldCompositor *compositor,
                                       GDestroyNotify destroy_notify,
                                       gpointer user_data,
                                       GError **error)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct wl_client *client;
  int fds[2];

  if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
    {
      int errsv = errno;

      g_set_error (error,
                   G_IO_ERROR,
                   g_io_error_from_errno (errsv),
                   _("Error creating wayland socketpair: %s"),
                   strerror (errsv));
      return -1;
    }

  client = wl_client_create (priv->wl_display, fds[0]);

  if (destroy_notify)
    {
      WakefieldClientDestroyListener *listener = g_new0 (WakefieldClientDestroyListener, 1);
      listener->listener.notify = client_destroyed;
      listener->destroy_notify = destroy_notify;
      listener->user_data = user_data;

      wl_client_add_destroy_listener (client, &listener->listener);
    }

  return fds[1];
}

static void
wakefield_compositor_finalize (GObject *object)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (object);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);

  g_source_destroy (priv->wayland_source);
  wl_display_destroy (priv->wl_display);

  G_OBJECT_CLASS (wakefield_compositor_parent_class)->finalize (object);
}


static void
wakefield_compositor_class_init (WakefieldCompositorClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = wakefield_compositor_finalize;

  widget_class->realize = wakefield_compositor_realize;
  widget_class->unrealize = wakefield_compositor_unrealize;
  widget_class->map = wakefield_compositor_map;
  widget_class->unmap = wakefield_compositor_unmap;
  widget_class->size_allocate = wakefield_compositor_size_allocate;
  widget_class->draw = wakefield_compositor_draw;
  widget_class->enter_notify_event = wakefield_compositor_enter_notify_event;
  widget_class->leave_notify_event = wakefield_compositor_leave_notify_event;
  widget_class->scroll_event = wakefield_compositor_scroll_event;
  widget_class->button_press_event = wakefield_compositor_button_press_event;
  widget_class->button_release_event = wakefield_compositor_button_release_event;
  widget_class->motion_notify_event = wakefield_compositor_motion_notify_event;
  widget_class->state_flags_changed = wakefield_compositor_state_flags_changed;
  widget_class->focus_in_event = wakefield_compositor_focus_in_event;
  widget_class->focus_out_event = wakefield_compositor_focus_out_event;
  widget_class->key_press_event = wakefield_compositor_key_press_event;
  widget_class->key_release_event = wakefield_compositor_key_release_event;
}

/* Wayland GSource */

typedef struct
{
  GSource source;
  struct wl_display *display;
} WaylandEventSource;

static gboolean
wayland_event_source_prepare (GSource *base, int *timeout)
{
  WaylandEventSource *source = (WaylandEventSource *)base;

  *timeout = -1;

  wl_display_flush_clients (source->display);

  return FALSE;
}

static gboolean
wayland_event_source_dispatch (GSource *base,
                               GSourceFunc callback,
                               void *data)
{
  WaylandEventSource *source = (WaylandEventSource *)base;
  struct wl_event_loop *loop = wl_display_get_event_loop (source->display);

  wl_event_loop_dispatch (loop, 0);

  return TRUE;
}

static GSourceFuncs wayland_event_source_funcs =
{
  wayland_event_source_prepare,
  NULL,
  wayland_event_source_dispatch,
  NULL
};

static GSource *
wayland_event_source_new (struct wl_display *display)
{
  WaylandEventSource *source;
  struct wl_event_loop *loop = wl_display_get_event_loop (display);

  source = (WaylandEventSource *) g_source_new (&wayland_event_source_funcs,
                                                sizeof (WaylandEventSource));
  source->display = display;
  g_source_add_unix_fd (&source->source,
                        wl_event_loop_get_fd (loop),
                        G_IO_IN | G_IO_ERR);

  return &source->source;
}
