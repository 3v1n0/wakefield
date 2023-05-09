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

#include "wakefield-compositor.h"

#include <wayland-server.h>

typedef struct _WakefieldSurface WakefieldSurface;

struct wl_display * wakefield_compositor_get_display            (WakefieldCompositor *compositor);
void                wakefield_compositor_surface_unmapped       (WakefieldCompositor *compositor,
                                                                 struct wl_resource  *surface);
void                wakefield_compositor_surface_mapped         (WakefieldCompositor *compositor,
                                                                 struct wl_resource  *surface);
void                wakefield_compositor_send_configure         (WakefieldCompositor *compositor,
                                                                 struct wl_resource  *surface);
void                wakefield_compositor_send_enter             (WakefieldCompositor *compositor,
                                                                 struct wl_resource  *surface,
                                                                 GdkEventCrossing     *event);
void                wakefield_compositor_send_leave             (WakefieldCompositor *compositor,
                                                                 struct wl_resource  *surface,
                                                                 GdkEventCrossing    *event);
void                wakefield_compositor_send_button            (WakefieldCompositor *compositor,
                                                                 struct wl_resource  *surface,
                                                                 GdkEventButton      *event);
void                wakefield_compositor_send_scroll            (WakefieldCompositor *compositor,
                                                                 struct wl_resource  *surface,
                                                                 GdkEventScroll      *event);
void                wakefield_compositor_send_motion            (WakefieldCompositor *compositor,
                                                                 struct wl_resource  *surface,
                                                                 GdkEventMotion      *event);

typedef enum {
  WAKEFIELD_SURFACE_ROLE_NONE,
  WAKEFIELD_SURFACE_ROLE_XDG_TOPLEVEL,
  WAKEFIELD_SURFACE_ROLE_XDG_POPUP,
  WAKEFIELD_SURFACE_ROLE_POINTER_CURSOR,
} WakefieldSurfaceRole;

struct wl_resource * wakefield_surface_new              (WakefieldCompositor *compositor,
                                                         struct wl_client    *client,
                                                         struct wl_resource  *compositor_resource,
                                                         uint32_t             id);
void                 wakefield_surface_draw             (struct wl_resource  *surface_resource,
                                                         cairo_t             *cr);
struct wl_resource * wakefield_surface_get_xdg_surface  (struct wl_resource  *surface_resource);
WakefieldSurfaceRole wakefield_surface_get_role         (struct wl_resource  *surface_resource);
void                 wakefield_surface_set_role         (struct wl_resource *surface_resource,
                                                         WakefieldSurfaceRole role);
GdkWindow *          wakefield_surface_get_window       (struct wl_resource  *surface_resource);
gboolean             wakefield_surface_is_mapped        (struct wl_resource  *surface_resource);

WakefieldCompositor *wakefield_surface_get_compositor   (WakefieldSurface *surface);
cairo_surface_t *    wakefield_surface_create_cairo_surface (WakefieldSurface *surface,
                                                             int              *width,
                                                             int              *height);

struct wl_resource *wakefield_xdg_surface_new (struct wl_client   *client,
                                               struct wl_resource *shell_resource,
                                               uint32_t            id,
                                               struct wl_resource *surface_resource);

WakefieldSurface   *wakefield_xdg_surface_get_surface (struct wl_resource *xdg_surface_resource);
struct wl_resource *wakefield_xdg_surface_get_surface_resource (struct wl_resource *xdg_surface_resource);
struct wl_resource *wakefield_xdg_surface_get_xdg_popup (struct wl_resource  *xdg_surface_resource);
struct wl_resource *wakefield_xdg_surface_get_xdg_toplevel (struct wl_resource  *xdg_surface_resource);
void                wakefield_xdg_surface_realize (struct wl_resource *xdg_surface_resource,
                                                   GdkWindow *parent);
void                wakefield_xdg_surface_unrealize (struct wl_resource *xdg_surface_resource);
GdkWindow *         wakefield_xdg_surface_get_window (struct wl_resource *xdg_surface_resource);

struct wl_resource *wakefield_xdg_popup_new (WakefieldCompositor *compositor,
                                             struct wl_client   *client,
                                             struct wl_resource *shell_resource,
                                             uint32_t            id,
                                             struct wl_resource *surface_resource,
                                             struct wl_resource *parent_resource,
                                             guint32 serial,
                                             gint32 x, gint32 y);

guint32             wakefield_xdg_popup_get_serial (struct wl_resource *xdg_popup_resource);
GdkWindow *         wakefield_xdg_popup_get_window (struct wl_resource *xdg_popup_resource);
void                wakefield_xdg_popup_close      (struct wl_resource *xdg_popup_resource);

cairo_region_t *wakefield_region_get_region (struct wl_resource *region_resource);

struct WakefieldDataDevice *wakefield_data_device_new (WakefieldCompositor *compositor);
