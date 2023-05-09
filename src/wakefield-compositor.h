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
 */

#pragma once

#include <gtk/gtk.h>

#define WAKEFIELD_TYPE_COMPOSITOR (wakefield_compositor_get_type ())

G_DECLARE_DERIVABLE_TYPE (WakefieldCompositor, wakefield_compositor,
                          WAKEFIELD, COMPOSITOR, GtkWidget);

struct _WakefieldCompositorClass
{
  GtkWidgetClass parent_class;
};

WakefieldCompositor *wakefield_compositor_new              (void);
const char *         wakefield_compositor_add_socket_auto  (WakefieldCompositor *compositor,
                                                            GError              **error);
gboolean             wakefield_compositor_add_socket       (WakefieldCompositor *compositor,
                                                            const char          *name,
                                                            GError              **error);
int                  wakefield_compositor_create_client_fd (WakefieldCompositor *compositor,
                                                            GDestroyNotify       destroy_notify,
                                                            gpointer             user_data,
                                                            GError             **error);
