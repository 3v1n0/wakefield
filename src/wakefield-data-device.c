/*
 * Copyright (C) 2015 Red Hat
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
 *     Alexander Larsson <alexl@redhat.com>
 */

#include <sys/time.h>

#include "wakefield-private.h"
#include "xdg-shell-server-protocol.h"

struct WakefieldDataDevice {
  WakefieldCompositor *compositor;

  struct wl_list data_source_resources;
  struct wl_list manager_resources;
  struct wl_list device_resources;
};

struct WakefieldDataSource {
  struct WakefieldDataDevice *data_device;
  struct wl_resource *resource;
};

static void
data_source_offer (struct wl_client *client,
                   struct wl_resource *source_resource,
                   const char *type)
{
  wl_resource_post_error (source_resource, 1,
                          "data-source:: offer not implemented yet.");
}


static void
data_source_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
data_source_set_actions (struct wl_client *client,
                         struct wl_resource *source_resource,
                         uint32_t dnd_actions)
{
  wl_resource_post_error (source_resource, 1,
                          "data-source:: set_actions not implemented yet.");
}

static struct wl_data_source_interface data_source_implementation = {
  data_source_offer,
  data_source_destroy,
  data_source_set_actions,
};

static void
data_source_finalize (struct wl_resource *resource)
{
  struct WakefieldDataSource *data_source = wl_resource_get_user_data (resource);

  wl_list_remove (wl_resource_get_link (resource));
  g_free (data_source);
}


static void
create_data_source (struct wl_client *client,
                    struct wl_resource *manager_resource,
                    uint32_t id)
{
  struct WakefieldDataDevice *data_device = wl_resource_get_user_data (manager_resource);
  struct WakefieldDataSource *data_source;

  data_source = g_new0 (struct WakefieldDataSource, 1);
  data_source->data_device = data_device;

  data_source->resource = wl_resource_create (client, &wl_data_source_interface, 1, id);
  wl_resource_set_implementation (data_source->resource, &data_source_implementation,
                                  data_source, data_source_finalize);
  wl_list_insert (&data_device->data_source_resources,
                  wl_resource_get_link (data_source->resource));
}

static void
data_device_start_drag (struct wl_client *client,
                        struct wl_resource *device_resource,
                        struct wl_resource *source_resource,
                        struct wl_resource *origin_resource,
                        struct wl_resource *icon_resource,
                        uint32_t serial)
{
  wl_resource_post_error (device_resource, 1,
                          "data-device:: start_drag not implemented yet.");
}

static void
data_device_set_selection (struct wl_client *client,
                           struct wl_resource *device_resource,
                           struct wl_resource *source_resource,
                           uint32_t serial)
{
  wl_resource_post_error (device_resource, 1,
                          "data-device:: set_selection not implemented yet.");
}

static void
data_device_release (struct wl_client *client,
                     struct wl_resource *device_resource)
{
  wl_resource_destroy (device_resource);
}

static const struct wl_data_device_interface data_device_implementation = {
  data_device_start_drag,
  data_device_set_selection,
  data_device_release
};

static void
data_device_finalize (struct wl_resource *resource)
{
  wl_list_remove (wl_resource_get_link (resource));
}

static void
get_data_device (struct wl_client *client,
                 struct wl_resource *manager_resource,
                 uint32_t id,
                 struct wl_resource *seat_resource)
{
  struct WakefieldDataDevice *data_device = wl_resource_get_user_data (manager_resource);
  struct wl_resource *device_resource;

  device_resource = wl_resource_create (client, &wl_data_device_interface,
                                        wl_resource_get_version(manager_resource), id);
  if (device_resource == NULL)
    {
      wl_resource_post_no_memory(manager_resource);
      return;
    }

  wl_list_insert (&data_device->device_resources,
                  wl_resource_get_link (device_resource));
  wl_resource_set_implementation (device_resource, &data_device_implementation,
                                  data_device, data_device_finalize);
}

static const struct wl_data_device_manager_interface manager_implementation = {
  create_data_source,
  get_data_device
};

static void
data_device_manager_finalize (struct wl_resource *resource)
{
  wl_list_remove (wl_resource_get_link (resource));
}

static void
bind_data_device_manager (struct wl_client *client,
                          void *data,
                          uint32_t version,
                          uint32_t id)
{
  struct WakefieldDataDevice *data_device = data;
  struct wl_resource *manager_resource;

  manager_resource = wl_resource_create (client, &wl_data_device_manager_interface, version, id);
  if (manager_resource == NULL)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (manager_resource, &manager_implementation, data_device, data_device_manager_finalize);
  wl_list_insert (&data_device->manager_resources,
                  wl_resource_get_link (manager_resource));
}

#define DATA_DEVICE_MANAGER_VERSION 2

struct WakefieldDataDevice *
wakefield_data_device_new (WakefieldCompositor *compositor)
{
  struct WakefieldDataDevice *data_device;

  data_device = g_new0 (struct WakefieldDataDevice, 1);
  data_device->compositor = compositor;

  wl_list_init (&data_device->manager_resources);
  wl_list_init (&data_device->data_source_resources);
  wl_list_init (&data_device->device_resources);

  wl_global_create (wakefield_compositor_get_display (compositor), &wl_data_device_manager_interface, DATA_DEVICE_MANAGER_VERSION,
                    data_device, bind_data_device_manager);

  return data_device;
}
