/* GStreamer decode backend.
 *
 * appsrc -> h26xparse -> decodebin -> counter -> sink. decodebin picks the
 * platform's hardware decoder itself — vah264dec on Intel, mppvideodec where
 * the GStreamer MPP plugin is installed — which is the whole reason this is the
 * portable backend: the same pipeline is the robust decoder on any platform
 * whose hardware decoder GStreamer knows about, and GStreamer's decoders
 * conceal a lost slice rather than aborting on it.
 */

#include <salvage/decoder.h>

#include "decoder_internal.h"

#include <string.h>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

typedef struct {
    GstElement *pipeline;
    GstElement *appsrc;
    guint64 frames;
    SalvageCodec codec;
    bool eos_pushed;
} GstDec;

static GstPadProbeReturn on_buffer(
    GstPad *pad, GstPadProbeInfo *info, gpointer user) {
    (void)pad;
    (void)info;
    ((GstDec *)user)->frames++;
    return GST_PAD_PROBE_OK;
}

static void pump_bus(GstDec *d) {
    GstBus *bus = gst_element_get_bus(d->pipeline);
    GstMessage *msg;
    while ((msg = gst_bus_pop_filtered(
                bus, GST_MESSAGE_ERROR | GST_MESSAGE_EOS)) != NULL) {
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
}

static void *gst_create(const SalvageDecoderConfig *cfg) {
    static gboolean inited = FALSE;
    if (!inited) {
        gst_init(NULL, NULL);
        inited = TRUE;
    }

    GstDec *d = g_new0(GstDec, 1);
    d->codec = cfg->codec;

    /* A file sink dumps decoded frames when asked; otherwise a fakesink just
     * lets the counter probe run. `sync=false` decodes as fast as it can. */
    gchar *tail;
    if (cfg->dump_yuv && cfg->out != NULL && strcmp(cfg->out, "-") != 0) {
        tail = g_strdup_printf(
            "videoconvert ! video/x-raw,format=I420 ! filesink location=%s",
            cfg->out);
    } else {
        tail = g_strdup("fakesink sync=false");
    }

    gchar *desc = g_strdup_printf(
        "appsrc name=src ! %s ! decodebin ! identity name=count ! %s",
        cfg->codec == SALVAGE_H265 ? "h265parse" : "h264parse", tail);
    g_free(tail);

    GError *err = NULL;
    d->pipeline = gst_parse_launch(desc, &err);
    g_free(desc);
    if (d->pipeline == NULL) {
        if (err != NULL) {
            g_error_free(err);
        }
        g_free(d);
        return NULL;
    }

    d->appsrc = gst_bin_get_by_name(GST_BIN(d->pipeline), "src");
    GstCaps *caps = gst_caps_new_simple(
        cfg->codec == SALVAGE_H265 ? "video/x-h265" : "video/x-h264",
        "stream-format", G_TYPE_STRING, "byte-stream", "alignment",
        G_TYPE_STRING, "au", NULL);
    gst_app_src_set_caps(GST_APP_SRC(d->appsrc), caps);
    gst_caps_unref(caps);
    g_object_set(d->appsrc, "format", GST_FORMAT_TIME, "is-live", FALSE, NULL);

    /* Count decoded frames as they pass the identity element. */
    GstElement *count = gst_bin_get_by_name(GST_BIN(d->pipeline), "count");
    GstPad *pad = gst_element_get_static_pad(count, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_buffer, d, NULL);
    gst_object_unref(pad);
    gst_object_unref(count);

    if (gst_element_set_state(d->pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        gst_object_unref(d->appsrc);
        gst_object_unref(d->pipeline);
        g_free(d);
        return NULL;
    }
    return d;
}

static int gst_feed(void *self, const uint8_t *annexb, size_t len) {
    GstDec *d = self;
    GstBuffer *buf = gst_buffer_new_allocate(NULL, len, NULL);
    gst_buffer_fill(buf, 0, annexb, len);
    /* A monotonically increasing timestamp keeps the parser and decoder
     * happy without asserting a real frame rate. */
    GST_BUFFER_PTS(buf) = gst_util_uint64_scale(d->frames, GST_SECOND, 30);
    if (gst_app_src_push_buffer(GST_APP_SRC(d->appsrc), buf) != GST_FLOW_OK) {
        return -1;
    }
    pump_bus(d);
    return (int)d->frames;
}

static int gst_finish(void *self) {
    GstDec *d = self;
    if (!d->eos_pushed) {
        gst_app_src_end_of_stream(GST_APP_SRC(d->appsrc));
        d->eos_pushed = true;
    }
    /* Drain: wait for EOS on the bus so late frames are counted. */
    GstBus *bus = gst_element_get_bus(d->pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
    if (msg != NULL) {
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
    return (int)d->frames;
}

static void gst_free(void *self) {
    GstDec *d = self;
    if (d == NULL) {
        return;
    }
    gst_element_set_state(d->pipeline, GST_STATE_NULL);
    if (d->appsrc != NULL) {
        gst_object_unref(d->appsrc);
    }
    gst_object_unref(d->pipeline);
    g_free(d);
}

const SalvageDecoderVtable salvage_decoder_gst_vt = {
    gst_create, gst_feed, gst_finish, gst_free};
