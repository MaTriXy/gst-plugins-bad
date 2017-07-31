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
 * SECTION:element-srtserversink
 * @title: srtserversink
 *
 * srtserversink is a network sink that sends <ulink url="http://www.srtalliance.org/">SRT</ulink>
 * packets to the network. Although SRT is an UDP-based protocol, srtserversink works like
 * a server socket of connection-oriented protocol.
 *
 * <refsect2>
 * <title>Examples</title>
 * |[
 * gst-launch-1.0 -v audiotestsrc ! srtserversink
 * ]| This pipeline shows how to serve SRT packets through the default port.
 * </refsect2>
 * 
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstsrtserversink.h"
#include <srt/srt.h>

#include <netinet/in.h>

#define SRT_URI_SCHEME "srt"
#define SRT_DEFAULT_PORT 7001
#define SRT_DEFAULT_HOST "127.0.0.1"
#define SRT_DEFAULT_URI SRT_URI_SCHEME"://"SRT_DEFAULT_HOST":"G_STRINGIFY(SRT_DEFAULT_PORT)
#define SRT_DEFAULT_POLL_TIMEOUT -1

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

#define GST_CAT_DEFAULT gst_debug_srt_server_sink
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

struct _GstSRTServerSinkPrivate
{
  GMutex mutex;

  SRTSOCKET sock;
  gint srt_poll_id;
  gint poll_timeout;
  GstUri *uri;

  GMainLoop *loop;
  GMainContext *context;
  GSource *server_source;
  GThread *thread;

  GList *clients;
  GList *queued_buffers;
};

#define GST_SRT_SERVER_SINK_GET_PRIVATE(obj)  \
       (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_SRT_SERVER_SINK, GstSRTServerSinkPrivate))

typedef enum
{
  PROP_URI = 1,
  PROP_POLL_TIMEOUT,
  /*< private > */
  PROP_LAST = PROP_POLL_TIMEOUT
} GstSRTServerSinkProperty;

static GParamSpec *properties[PROP_LAST + 1];

enum
{
  SIG_CLIENT_ADDED,
  SIG_CLIENT_REMOVED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void gst_srt_server_sink_uri_handler_init (gpointer g_iface,
    gpointer iface_data);
static gchar *gst_srt_server_sink_uri_get_uri (GstURIHandler * handler);
static gboolean gst_srt_server_sink_uri_set_uri (GstURIHandler * handler,
    const gchar * uri, GError ** error);

#define gst_srt_server_sink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstSRTServerSink, gst_srt_server_sink,
    GST_TYPE_BASE_SINK, G_ADD_PRIVATE (GstSRTServerSink)
    G_IMPLEMENT_INTERFACE (GST_TYPE_URI_HANDLER,
        gst_srt_server_sink_uri_handler_init)
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "srtserversink", 0,
        "SRT Server Sink"));

static void
srt_emit_client_removed (GstSRTClient * client, gpointer user_data)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (user_data);
  g_return_if_fail (client != NULL && GST_IS_SRT_SERVER_SINK (self));

  g_signal_emit (self, signals[SIG_CLIENT_REMOVED], 0, client->sock,
      client->addr);
}

static void
gst_srt_server_sink_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (object);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);

  switch ((GstSRTServerSinkProperty) prop_id) {
    case PROP_URI:
      if (priv->uri != NULL) {
        gchar *uri_str =
            gst_srt_server_sink_uri_get_uri (GST_URI_HANDLER (self));
        g_value_take_string (value, uri_str);
      }
      break;
    case PROP_POLL_TIMEOUT:
      g_value_set_int (value, priv->poll_timeout);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_srt_server_sink_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (object);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);

  switch ((GstSRTServerSinkProperty) prop_id) {
    case PROP_URI:
      gst_srt_server_sink_uri_set_uri (GST_URI_HANDLER (self),
          g_value_get_string (value), NULL);
      break;
    case PROP_POLL_TIMEOUT:
      priv->poll_timeout = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_srt_server_sink_finalize (GObject * object)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (object);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);

  g_clear_pointer (&priv->uri, gst_uri_unref);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
idle_listen_callback (gpointer data)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (data);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);
  gboolean ret = TRUE;

  GstSRTClient *srt_client;
  struct sockaddr sa;
  gint sa_len;
  SRTSOCKET client_sock;
  SRTSOCKET ready[2];
  GInetAddress *client_addr;

  if (srt_epoll_wait (priv->srt_poll_id, ready, &(int) {
          2}, 0, 0, priv->poll_timeout, 0, 0, 0, 0) == -1) {

    /* Assuming that timeout error is normal */
    if (srt_getlasterror (NULL) != SRT_ETIMEOUT) {
      GST_DEBUG_OBJECT (self, "%s", srt_getlasterror_str ());
      ret = FALSE;
    }
    srt_clearlasterror ();
    goto out;
  }

  client_sock = srt_accept (priv->sock, &sa, &sa_len);

  if (client_sock == SRT_INVALID_SOCK) {
    GST_WARNING_OBJECT (self, "detected invalid SRT client socket (reason: %s)",
        srt_getlasterror_str ());
    srt_clearlasterror ();
    ret = FALSE;
    goto out;
  }

  client_addr =
      g_inet_address_new_from_bytes ((const guint8 *) sa.sa_data, sa.sa_family);
  srt_client = gst_srt_client_new (client_sock, client_addr);

  priv->clients = g_list_append (priv->clients, srt_client);
  g_signal_emit (self, signals[SIG_CLIENT_ADDED], 0, srt_client->sock,
      client_addr);
  GST_DEBUG_OBJECT (self, "client added");
  g_object_unref (client_addr);
out:
  return ret;
}

static gpointer
thread_func (gpointer data)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (data);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);

  g_main_loop_run (priv->loop);

  return NULL;
}

static gboolean
gst_srt_server_sink_start (GstBaseSink * sink)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);
  gboolean ret = TRUE;
  const gchar *host = gst_uri_get_host (priv->uri);
  guint port = gst_uri_get_port (priv->uri);
  GInetAddress *inet_addr;
  GInetSocketAddress *sock_addr = NULL;
  int srt_stat;
  struct sockaddr_in sa;
  struct sockaddr_in6 sa6;
  GError *error = NULL;

  if (host == NULL || port == GST_URI_NO_PORT) {
    GST_WARNING_OBJECT (self,
        "failed to extract host or port from the given URI");
    ret = FALSE;
    goto out;
  }

  /* binding SRT socket */
  sock_addr =
      G_INET_SOCKET_ADDRESS (g_inet_socket_address_new_from_string (host,
          port));

  if (sock_addr == NULL) {
    GST_WARNING_OBJECT (self, "failed to parse host string(%s)", host);
    ret = FALSE;
    goto out;
  }
  inet_addr = g_inet_socket_address_get_address (sock_addr);

  priv->sock = srt_socket (g_inet_address_get_family (inet_addr),
      G_SOCKET_TYPE_DATAGRAM, 0);
  if (priv->sock == SRT_ERROR) {
    GST_WARNING_OBJECT (self, "failed to create SRT socket (reason: %s)",
        srt_getlasterror_str ());
    ret = FALSE;
    goto out;
  }

  /* Make SRT non-blocking */
  srt_setsockopt (priv->sock, 0, SRTO_SNDSYN, &(int) {
      0}, sizeof (int));

  priv->srt_poll_id = srt_epoll_create ();
  if (priv->srt_poll_id == -1) {
    GST_WARNING_OBJECT (self,
        "failed to create poll id for SRT socket (reason: %s)",
        srt_getlasterror_str ());
    srt_close (priv->sock);
    goto out;
  }
  srt_epoll_add_usock (priv->srt_poll_id, priv->sock, &(int) {
      SRT_EPOLL_IN});

  switch (g_inet_address_get_family (inet_addr)) {
    case G_SOCKET_FAMILY_IPV4:
      memset (&sa, 0, sizeof (sa));
      sa.sin_family = g_inet_address_get_family (inet_addr);
      sa.sin_port = htons (g_inet_socket_address_get_port (sock_addr));
      memcpy (&sa.sin_addr, g_inet_address_to_bytes (inet_addr),
          sizeof (struct in_addr));
      srt_stat = srt_bind (priv->sock, (struct sockaddr *) &sa, sizeof (sa));
      GST_INFO_OBJECT (self, "binding IPv4 address");
      break;
    case G_SOCKET_FAMILY_IPV6:
      memset (&sa6, 0, sizeof (sa6));
      sa6.sin6_family = g_inet_address_get_family (inet_addr);
      sa6.sin6_port = htons (g_inet_socket_address_get_port (sock_addr));
      memcpy (&sa6.sin6_addr, g_inet_address_to_bytes (inet_addr),
          sizeof (struct in6_addr));
      srt_stat = srt_bind (priv->sock, (struct sockaddr *) &sa6, sizeof (sa6));
      GST_INFO_OBJECT (self, "binding IPv6 address");
      break;
    default:
      GST_WARNING_OBJECT (self, "SRT supports only IPV4 or IPV6");
      ret = FALSE;
      srt_close (priv->sock);
      goto out;
  }

  if (srt_stat == SRT_ERROR) {
    GST_WARNING_OBJECT (self, "failed to bind socket (reason: %s)",
        srt_getlasterror_str ());
    ret = FALSE;
    srt_close (priv->sock);
    goto out;
  }

  if (srt_listen (priv->sock, 1) == SRT_ERROR) {
    GST_WARNING_OBJECT (self, "failed to bind socket (reason: %s)",
        srt_getlasterror_str ());
    ret = FALSE;
    srt_close (priv->sock);
    goto out;
  }

  priv->context = g_main_context_new ();

  priv->server_source = g_idle_source_new ();
  g_source_set_callback (priv->server_source,
      (GSourceFunc) idle_listen_callback, gst_object_ref (self),
      (GDestroyNotify) gst_object_unref);

  g_source_attach (priv->server_source, priv->context);
  priv->loop = g_main_loop_new (priv->context, TRUE);

  g_mutex_lock (&priv->mutex);

  priv->thread = g_thread_try_new ("srtserversink", thread_func, self, &error);
  if (error != NULL) {
    GST_WARNING_OBJECT (self, "failed to create thread (reason: %s)",
        error->message);
    ret = FALSE;
  }

  g_mutex_unlock (&priv->mutex);
out:
  g_clear_error (&error);
  return ret;
}

static gboolean
srt_send_buffer (GstSRTServerSink * self)
{
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);
  GList *buffers = priv->queued_buffers;
  GList *clients = priv->clients;
  GstMapInfo info;

  GST_TRACE_OBJECT (self, "invoked sending buffers");

  g_mutex_lock (&priv->mutex);
  for (; buffers; buffers = buffers->next) {
    GstBuffer *buffer = buffers->data;

    GST_TRACE_OBJECT (self, "received buffer %p, offset %"
        G_GINT64_FORMAT ", offset_end %" G_GINT64_FORMAT
        ", timestamp %" GST_TIME_FORMAT ", duration %" GST_TIME_FORMAT
        ", size %" G_GSIZE_FORMAT,
        buffer, GST_BUFFER_OFFSET (buffer),
        GST_BUFFER_OFFSET_END (buffer),
        GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buffer)),
        GST_TIME_ARGS (GST_BUFFER_DURATION (buffer)),
        gst_buffer_get_size (buffer));

    if (!gst_buffer_map (buffer, &info, GST_MAP_READ)) {
      GST_ELEMENT_ERROR (self, RESOURCE, READ,
          ("Could not map the input stream"), (NULL));
      continue;
    }

    while (clients != NULL) {
      GstSRTClient *client = clients->data;
      clients = clients->next;

      if (srt_sendmsg2 (client->sock, (char *) info.data, info.size,
              0) == SRT_ERROR) {
        GST_WARNING_OBJECT (self, "%s", srt_getlasterror_str ());

        priv->clients = g_list_remove (priv->clients, client);
        g_signal_emit (self, signals[SIG_CLIENT_REMOVED], 0, client->sock,
            client->addr);
        gst_srt_client_unref (client);
      }
    }

    gst_buffer_unmap (buffer, &info);
  }

  g_list_free_full (priv->queued_buffers, (GDestroyNotify) gst_buffer_unref);

  priv->queued_buffers = NULL;
  g_mutex_unlock (&priv->mutex);

  return FALSE;
}

static GstFlowReturn
gst_srt_server_sink_render (GstBaseSink * sink, GstBuffer * buffer)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);
  GSource *source;

  g_mutex_lock (&priv->mutex);
  if (priv->queued_buffers == NULL) {
    priv->queued_buffers =
        g_list_append (priv->queued_buffers, gst_buffer_ref (buffer));

    source = g_idle_source_new ();
    g_source_set_callback (source, (GSourceFunc) srt_send_buffer,
        gst_object_ref (self), (GDestroyNotify) gst_object_unref);
    g_source_attach (source, NULL);
    g_source_unref (source);
  }

  g_mutex_unlock (&priv->mutex);

  return GST_FLOW_OK;
}

static gboolean
gst_srt_server_sink_stop (GstBaseSink * sink)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (sink);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);
  gboolean ret = TRUE;

  GST_DEBUG_OBJECT (self, "closing client sockets");
  g_list_foreach (priv->clients, (GFunc) srt_emit_client_removed, self);
  g_list_free_full (priv->clients, (GDestroyNotify) gst_srt_client_unref);

  GST_DEBUG_OBJECT (self, "closing SRT connection");
  srt_epoll_remove_usock (priv->srt_poll_id, priv->sock);
  srt_epoll_release (priv->srt_poll_id);
  srt_close (priv->sock);

  if (priv->loop) {
    g_main_loop_quit (priv->loop);
    g_thread_join (priv->thread);
    g_clear_pointer (&priv->loop, g_main_loop_unref);
    g_clear_pointer (&priv->thread, g_thread_unref);
  }

  if (priv->server_source) {
    g_source_destroy (priv->server_source);
    g_clear_pointer (&priv->server_source, g_source_unref);
  }

  g_clear_pointer (&priv->context, g_main_context_unref);

  return ret;
}

static void
gst_srt_server_sink_class_init (GstSRTServerSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *gstbasesink_class = GST_BASE_SINK_CLASS (klass);

  gobject_class->set_property = gst_srt_server_sink_set_property;
  gobject_class->get_property = gst_srt_server_sink_get_property;
  gobject_class->finalize = gst_srt_server_sink_finalize;

  /**
   * GstSRTServerSink:uri:
   * 
   * The URI used by SRT Connection.
   */
  properties[PROP_URI] = g_param_spec_string ("uri", "URI",
      "URI in the form of srt://address:port", SRT_DEFAULT_URI,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_POLL_TIMEOUT] =
      g_param_spec_int ("poll-timeout", "Poll Timeout",
      "Return poll wait after timeout miliseconds (-1 = infinite)", G_MININT32,
      G_MAXINT32, SRT_DEFAULT_POLL_TIMEOUT,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, G_N_ELEMENTS (properties),
      properties);

  /**
   * GstSRTServerSink::client-added:
   * @gstsrtserversink: the srtserversink element that emitted this signal
   * @sock: the client socket descriptor that was added to srtserversink
   * @addr: the #GInetAddress object that describes the socket descriptor
   * 
   * The given socket descriptor was added to srtserversink.
   */
  signals[SIG_CLIENT_ADDED] =
      g_signal_new ("client-added", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstSRTServerSinkClass, client_added),
      NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE, 2,
      G_TYPE_INT, G_TYPE_INET_ADDRESS);

  /**
   * GstSRTServerSink::client-removed:
   * @gstsrtserversink: the srtserversink element that emitted this signal
   * @sock: the client socket descriptor that was added to srtserversink
   * @addr: the #GInetAddress object that describes the socket descriptor
   *
   * The given socket descriptor was removed to srtserversink.
   */
  signals[SIG_CLIENT_REMOVED] =
      g_signal_new ("client-removed", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstSRTServerSinkClass,
          client_removed), NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE,
      2, G_TYPE_INT, G_TYPE_INET_ADDRESS);

  gst_element_class_add_static_pad_template (gstelement_class, &sink_template);
  gst_element_class_set_metadata (gstelement_class,
      "SRT server sink", "Sink/Network",
      "Send data over the network via SRT",
      "Justin Kim <justin.kim@collabora.com>");

  gstbasesink_class->start = GST_DEBUG_FUNCPTR (gst_srt_server_sink_start);
  gstbasesink_class->stop = GST_DEBUG_FUNCPTR (gst_srt_server_sink_stop);
  gstbasesink_class->render = GST_DEBUG_FUNCPTR (gst_srt_server_sink_render);
}

static void
gst_srt_server_sink_init (GstSRTServerSink * self)
{
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);
  priv->uri = gst_uri_from_string (SRT_DEFAULT_URI);
  priv->queued_buffers = NULL;
  priv->poll_timeout = SRT_DEFAULT_POLL_TIMEOUT;
  g_mutex_init (&priv->mutex);
}

static GstURIType
gst_srt_server_sink_uri_get_type (GType type)
{
  return GST_URI_SINK;
}

static const gchar *const *
gst_srt_server_sink_uri_get_protocols (GType type)
{
  static const gchar *protocols[] = { SRT_URI_SCHEME, NULL };

  return protocols;
}

static gchar *
gst_srt_server_sink_uri_get_uri (GstURIHandler * handler)
{
  gchar *uri_str;
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (handler);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);

  GST_OBJECT_LOCK (self);
  uri_str = gst_uri_to_string (priv->uri);
  GST_OBJECT_UNLOCK (self);

  return uri_str;
}

static gboolean
gst_srt_server_sink_uri_set_uri (GstURIHandler * handler,
    const gchar * uri, GError ** error)
{
  GstSRTServerSink *self = GST_SRT_SERVER_SINK (handler);
  GstSRTServerSinkPrivate *priv = GST_SRT_SERVER_SINK_GET_PRIVATE (self);
  gboolean ret = TRUE;
  GstUri *parsed_uri = gst_uri_from_string (uri);

  GST_TRACE_OBJECT (self, "Requested URI=%s", uri);

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
gst_srt_server_sink_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  iface->get_type = gst_srt_server_sink_uri_get_type;
  iface->get_protocols = gst_srt_server_sink_uri_get_protocols;
  iface->get_uri = gst_srt_server_sink_uri_get_uri;
  iface->set_uri = gst_srt_server_sink_uri_set_uri;
}
