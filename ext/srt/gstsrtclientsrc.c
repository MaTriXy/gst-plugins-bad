/* GStreamer SRT plugin based on libsrt
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

/**
 * SECTION:element-srtclientsrc
 * @title: srtclientsrc
 *
 * srtclientsrc is a network source that reads <ulink url="http://www.srtalliance.org/">SRT</ulink>
 * packets from the network. Although SRT is a protocol based on UDP, srtclientsrc works like
 * a client socket of connection-oriented protocol.
 *
 * <refsect2>
 * <title>Examples</title>
 * |[
 * gst-launch-1.0 -v srtclientsrc uri="srt://127.0.0.1:7001" ! fakesink
 * ]| This pipeline shows how to connect SRT server by setting #GstSRTClientSrc:uri property. 
 * </refsect2>
 * 
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstsrtclientsrc.h"
#include <srt/srt.h>
#include <gio/gio.h>

#include <netinet/in.h>

#define SRT_URI_SCHEME "srt"
#define SRT_DEFAULT_PORT 7000
#define SRT_DEFAULT_HOST "127.0.0.1"
#define SRT_DEFAULT_URI SRT_URI_SCHEME"://"SRT_DEFAULT_HOST":"G_STRINGIFY(SRT_DEFAULT_PORT)

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

#define GST_CAT_DEFAULT gst_debug_srt_client_src
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

struct _GstSrtClientSrcPrivate
{
  SRTSOCKET srt_sock;
  gint srt_poll_id;
  GstUri *uri;
  GstCaps *caps;
};

#define GST_SRT_CLIENT_SRC_GET_PRIVATE(obj)  \
       (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_SRT_CLIENT_SRC, GstSrtClientSrcPrivate))

typedef enum
{
  PROP_URI = 1,
  PROP_CAPS,

  /*< private > */
  PROP_LAST = PROP_CAPS
} GstSrtClientSrcProperty;

static GParamSpec *properties[PROP_LAST + 1];

static void gst_srt_client_src_uri_handler_init (gpointer g_iface,
    gpointer iface_data);
static gchar *gst_srt_client_src_uri_get_uri (GstURIHandler * handler);
static gboolean gst_srt_client_src_uri_set_uri (GstURIHandler * handler,
    const gchar * uri, GError ** error);

#define gst_srt_client_src_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstSrtClientSrc, gst_srt_client_src, GST_TYPE_PUSH_SRC,
    G_ADD_PRIVATE (GstSrtClientSrc)
    G_IMPLEMENT_INTERFACE (GST_TYPE_URI_HANDLER,
        gst_srt_client_src_uri_handler_init)
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "srtclientsrc", 0,
        "SRT Client Source"));

static void
gst_srt_client_src_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstSrtClientSrc *self = GST_SRT_CLIENT_SRC (object);
  GstSrtClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE (self);

  switch (prop_id) {
    case PROP_URI:
      if (priv->uri != NULL) {
        gchar *uri_str =
            gst_srt_client_src_uri_get_uri (GST_URI_HANDLER (self));
        g_value_take_string (value, uri_str);
      }
      break;
    case PROP_CAPS:
      GST_OBJECT_LOCK (self);
      gst_value_set_caps (value, priv->caps);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_srt_client_src_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstSrtClientSrc *self = GST_SRT_CLIENT_SRC (object);
  GstSrtClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE (self);

  switch (prop_id) {
    case PROP_URI:
      gst_srt_client_src_uri_set_uri (GST_URI_HANDLER (self),
          g_value_get_string (value), NULL);
      break;
    case PROP_CAPS:
      GST_OBJECT_LOCK (self);
      g_clear_pointer (&priv->caps, gst_caps_unref);
      priv->caps = gst_caps_copy (gst_value_get_caps (value));
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_srt_client_src_finalize (GObject * object)
{
  GstSrtClientSrc *self = GST_SRT_CLIENT_SRC (object);
  GstSrtClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE (self);

  g_clear_pointer (&priv->uri, gst_uri_unref);
  g_clear_pointer (&priv->caps, gst_caps_unref);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstCaps *
gst_srt_client_src_get_caps (GstBaseSrc * src, GstCaps * filter)
{
  GstSrtClientSrc *self = GST_SRT_CLIENT_SRC (src);
  GstSrtClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE (self);
  GstCaps *result, *caps = NULL;

  GST_OBJECT_LOCK (self);
  if (priv->caps != NULL) {
    caps = gst_caps_ref (priv->caps);
  }
  GST_OBJECT_UNLOCK (self);

  if (caps) {
    if (filter) {
      result = gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
      gst_caps_unref (caps);
    } else {
      result = caps;
    }
  } else {
    result = (filter) ? gst_caps_ref (filter) : gst_caps_new_any ();
  }

  return result;
}

static GstFlowReturn
gst_srt_client_src_fill (GstPushSrc * src, GstBuffer * outbuf)
{
  GstSrtClientSrc *self = GST_SRT_CLIENT_SRC (src);
  GstSrtClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE (self);
  GstFlowReturn ret = GST_FLOW_OK;
  GstMapInfo info;
  SRTSOCKET ready[2];
  gint recv_len;

  if (srt_epoll_wait (priv->srt_poll_id, 0, 0, ready, &(int) {
          2}, -1, 0, 0, 0, 0) == -1) {
    GST_DEBUG_OBJECT (self, "%s", srt_getlasterror_str ());
    ret = GST_FLOW_ERROR;
    goto out;
  }

  if (!gst_buffer_map (outbuf, &info, GST_MAP_WRITE)) {
    GST_ELEMENT_ERROR (src, RESOURCE, WRITE,
        ("Could not map the output stream"), (NULL));
    ret = GST_FLOW_ERROR;
    goto out;
  }

  recv_len = srt_recvmsg (priv->srt_sock, (char *) info.data,
      gst_buffer_get_size (outbuf));

  gst_buffer_unmap (outbuf, &info);

  if (recv_len == SRT_ERROR) {
    GST_WARNING_OBJECT (self, "%s", srt_getlasterror_str ());
    ret = GST_FLOW_ERROR;
    goto out;
  } else if (recv_len == 0) {
    ret = GST_FLOW_EOS;
    goto out;
  }

  GST_BUFFER_PTS (outbuf) =
      gst_clock_get_time (GST_ELEMENT_CLOCK (src)) -
      GST_ELEMENT_CAST (src)->base_time;

  gst_buffer_resize (outbuf, 0, recv_len);

  GST_LOG_OBJECT (src,
      "filled buffer from _get of size %" G_GSIZE_FORMAT ", ts %"
      GST_TIME_FORMAT ", dur %" GST_TIME_FORMAT
      ", offset %" G_GINT64_FORMAT ", offset_end %" G_GINT64_FORMAT,
      gst_buffer_get_size (outbuf),
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (outbuf)),
      GST_TIME_ARGS (GST_BUFFER_DURATION (outbuf)),
      GST_BUFFER_OFFSET (outbuf), GST_BUFFER_OFFSET_END (outbuf));

out:
  return ret;
}

static gboolean
gst_srt_client_src_start (GstBaseSrc * src)
{
  GstSrtClientSrc *self = GST_SRT_CLIENT_SRC (src);
  GstSrtClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE (self);
  gboolean ret = TRUE;
  const gchar *host = gst_uri_get_host (priv->uri);
  guint port = gst_uri_get_port (priv->uri);
  GInetAddress *inet_addr;
  GInetSocketAddress *sock_addr = NULL;
  int srt_stat;
  struct sockaddr_in sa;
  struct sockaddr_in6 sa6;

  if (host == NULL || port == GST_URI_NO_PORT) {
    GST_WARNING_OBJECT (self,
        "failed to extract host or port from the given URI");
    ret = FALSE;
    goto out;
  }

  sock_addr =
      G_INET_SOCKET_ADDRESS (g_inet_socket_address_new_from_string (host,
          port));
  if (sock_addr == NULL) {
    GST_WARNING_OBJECT (self, "failed to parse host string(%s)", host);
    ret = FALSE;
    goto out;
  }
  inet_addr = g_inet_socket_address_get_address (sock_addr);

  priv->srt_sock = srt_socket (g_inet_address_get_family (inet_addr),
      G_SOCKET_TYPE_DATAGRAM, 0);
  if (priv->srt_sock == SRT_ERROR) {
    GST_WARNING_OBJECT (self, "failed to create SRT socket (reason: %s)",
        srt_getlasterror_str ());
    ret = FALSE;
    goto out;
  }

  priv->srt_poll_id = srt_epoll_create ();

  if (priv->srt_poll_id == -1) {
    GST_WARNING_OBJECT (self,
        "failed to create poll id for SRT socket (reason: %s)",
        srt_getlasterror_str ());
    srt_close (priv->srt_sock);
    goto out;
  }

  srt_epoll_add_usock (priv->srt_poll_id, priv->srt_sock, &(int) {
      SRT_EPOLL_OUT});

  switch (g_inet_address_get_family (inet_addr)) {
    case G_SOCKET_FAMILY_IPV4:
      memset (&sa, 0, sizeof (sa));
      sa.sin_family = g_inet_address_get_family (inet_addr);
      sa.sin_port = htons (g_inet_socket_address_get_port (sock_addr));
      memcpy (&sa.sin_addr, g_inet_address_to_bytes (inet_addr),
          sizeof (struct in_addr));
      srt_stat =
          srt_connect (priv->srt_sock, (struct sockaddr *) &sa, sizeof (sa));
      break;
    case G_SOCKET_FAMILY_IPV6:
      memset (&sa6, 0, sizeof (sa6));
      sa6.sin6_family = g_inet_address_get_family (inet_addr);
      sa6.sin6_port = htons (g_inet_socket_address_get_port (sock_addr));
      memcpy (&sa6.sin6_addr, g_inet_address_to_bytes (inet_addr),
          sizeof (struct in6_addr));
      srt_stat =
          srt_connect (priv->srt_sock, (struct sockaddr *) &sa6, sizeof (sa6));
      break;
    default:
      GST_WARNING_OBJECT (self, "SRT supports only IPV4 or IPV6");
      ret = FALSE;
      srt_close (priv->srt_sock);
      goto out;
  }

  if (srt_stat == SRT_ERROR) {
    GST_WARNING_OBJECT (self, "failed to connect to host[%s:%d] (reason: %s)",
        host, port, srt_getlasterror_str ());
    ret = FALSE;
    srt_close (priv->srt_sock);
    goto out;
  }

out:
  g_clear_object (&sock_addr);

  return ret;
}

static gboolean
gst_srt_client_src_stop (GstBaseSrc * src)
{
  GstSrtClientSrc *self = GST_SRT_CLIENT_SRC (src);
  GstSrtClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE (self);

  srt_epoll_remove_usock (priv->srt_poll_id, priv->srt_sock);
  srt_epoll_release (priv->srt_poll_id);

  GST_DEBUG_OBJECT (self, "closing SRT connection");
  srt_close (priv->srt_sock);

  return TRUE;
}

static void
gst_srt_client_src_class_init (GstSrtClientSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS (klass);
  GstPushSrcClass *gstpushsrc_class = GST_PUSH_SRC_CLASS (klass);

  gobject_class->set_property = gst_srt_client_src_set_property;
  gobject_class->get_property = gst_srt_client_src_get_property;
  gobject_class->finalize = gst_srt_client_src_finalize;

  /**
   * GstSrtClientSrc:uri:
   * 
   * The URI used by SRT Connection.
   */
  properties[PROP_URI] = g_param_spec_string ("uri", "URI",
      "URI in the form of srt://address:port", SRT_DEFAULT_URI,
      G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS);

  /**
   * GstSrtClientSrc:caps:
   *
   * The Caps used by the source pad.
   */
  properties[PROP_CAPS] =
      g_param_spec_boxed ("caps", "Caps", "The caps of the source pad",
      GST_TYPE_CAPS, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, G_N_ELEMENTS (properties),
      properties);

  gst_element_class_add_static_pad_template (gstelement_class, &src_template);
  gst_element_class_set_metadata (gstelement_class,
      "SRT client source", "Source/Network",
      "Receive data over the network via SRT",
      "Justin Kim <justin.kim@collabora.com>");

  gstbasesrc_class->get_caps = GST_DEBUG_FUNCPTR (gst_srt_client_src_get_caps);
  gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_srt_client_src_start);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_srt_client_src_stop);

  gstpushsrc_class->fill = GST_DEBUG_FUNCPTR (gst_srt_client_src_fill);
}

static void
gst_srt_client_src_init (GstSrtClientSrc * self)
{
  gst_srt_client_src_uri_set_uri (GST_URI_HANDLER (self), SRT_DEFAULT_URI,
      NULL);
  gst_base_src_set_format (GST_BASE_SRC (self), GST_FORMAT_TIME);
  gst_base_src_set_live (GST_BASE_SRC (self), TRUE);
}

static GstURIType
gst_srt_client_src_uri_get_type (GType type)
{
  return GST_URI_SRC;
}

static const gchar *const *
gst_srt_client_src_uri_get_protocols (GType type)
{
  static const gchar *protocols[] = { SRT_URI_SCHEME, NULL };

  return protocols;
}

static gchar *
gst_srt_client_src_uri_get_uri (GstURIHandler * handler)
{
  gchar *uri_str;
  GstSrtClientSrc *self = GST_SRT_CLIENT_SRC (handler);
  GstSrtClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE (self);

  GST_OBJECT_LOCK (self);
  uri_str = gst_uri_to_string (priv->uri);
  GST_OBJECT_UNLOCK (self);

  return uri_str;
}

static gboolean
gst_srt_client_src_uri_set_uri (GstURIHandler * handler,
    const gchar * uri, GError ** error)
{
  GstSrtClientSrc *self = GST_SRT_CLIENT_SRC (handler);
  GstSrtClientSrcPrivate *priv = GST_SRT_CLIENT_SRC_GET_PRIVATE (self);
  gboolean ret = TRUE;
  GstUri *parsed_uri = gst_uri_from_string (uri);

  GST_TRACE ("setting URI to [%s]", uri);

  if (g_strcmp0 (gst_uri_get_scheme (parsed_uri), SRT_URI_SCHEME) != 0) {
    g_set_error (error, GST_URI_ERROR, GST_URI_ERROR_BAD_URI,
        "Invalid SRT URI scheme");
    ret = FALSE;
    goto out;
  }

  GST_OBJECT_LOCK (self);

  g_clear_pointer (&priv->uri, gst_uri_unref);
  priv->uri = gst_uri_ref (parsed_uri);

  GST_OBJECT_UNLOCK (self);

out:
  g_clear_pointer (&parsed_uri, gst_uri_unref);
  return ret;
}

static void
gst_srt_client_src_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  iface->get_type = gst_srt_client_src_uri_get_type;
  iface->get_protocols = gst_srt_client_src_uri_get_protocols;
  iface->get_uri = gst_srt_client_src_uri_get_uri;
  iface->set_uri = gst_srt_client_src_uri_set_uri;
}
