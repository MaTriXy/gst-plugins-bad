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

#ifndef __GST_SRT_H__
#define __GST_SRT_H__

#include <gst/gst.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
  int sock;
  GInetAddress *addr;

  /*< private >*/
  guint ref_count;
} GstSRTClient;

GstSRTClient * gst_srt_client_new (int sock, GInetAddress *addr);
GstSRTClient * gst_srt_client_ref (GstSRTClient * client);
void gst_srt_client_unref (GstSRTClient * client);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GstSRTClient, gst_srt_client_unref);

G_END_DECLS

#endif /* __GST_SRT_H__ */

