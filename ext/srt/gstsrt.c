/* GStreamer
 * Copyright (C) 2017 Justin Kim <justin.kim@collabora.com>
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstsrt.h"
#include "gstsrtclientsrc.h"
#include "gstsrtserversink.h"

#include <srt/srt.h>

GstSRTClient *
gst_srt_client_new (int sock, GInetAddress * addr)
{
  GstSRTClient *client;
  g_return_val_if_fail ((sock != -1) && (addr != NULL), NULL);

  client = g_new0 (GstSRTClient, 1);
  client->ref_count = 1;
  client->sock = sock;
  client->addr = g_object_ref (addr);

  return client;
}

GstSRTClient *
gst_srt_client_ref (GstSRTClient * client)
{
  g_return_val_if_fail (client != NULL, NULL);

  client->ref_count++;
  return client;
}

void
gst_srt_client_unref (GstSRTClient * client)
{
  g_return_if_fail (client != NULL);

  client->ref_count--;

  if (client->ref_count > 0)
    return;

  g_clear_object (&client->addr);
  srt_close (client->sock);
  g_free (client);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  if (!gst_element_register (plugin, "srtclientsrc", GST_RANK_PRIMARY,
          GST_TYPE_SRT_CLIENT_SRC))
    return FALSE;

  if (!gst_element_register (plugin, "srtserversink", GST_RANK_PRIMARY,
          GST_TYPE_SRT_SERVER_SINK))
    return FALSE;

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    srt,
    "transfer data via SRT",
    plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN);
