plugin_LTLIBRARIES = libgstsrt.la

libgstsrt_la_SOURCES = \
	gstsrt.c \
	gstsrtclientsrc.c \
	gstsrtclientsrc.h \
	gstsrtserversink.c \
	gstsrtserversink.h \
	$(NULL)

libgstsrt_la_CFLAGS = \
    $(GST_PLUGINS_BASE_CFLAGS) \
    $(GST_CFLAGS) \
    $(GIO_CFLAGS) \
    $(SRT_CFLAGS) \
    $(NULL)

libgstsrt_la_LIBADD = \
    $(GST_PLUGINS_BASE_LIBS) \
	$(GST_LIBS) \
	$(GIO_LIBS) \
	$(SRT_LIBS) \
    $(NULL)

libgstsrt_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)

CLEANFILES = $(BUILT_SOURCES)

include $(top_srcdir)/common/gst-glib-gen.mak
