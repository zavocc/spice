/* spice-server character device to handle a video stream

   Copyright (C) 2017 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <spice/stream-device.h>

#include "char-device.h"
#include "stream-channel.h"
#include "cursor-channel.h"
#include "reds.h"

#define TYPE_STREAM_DEVICE stream_device_get_type()

#define STREAM_DEVICE(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_STREAM_DEVICE, StreamDevice))
#define STREAM_DEVICE_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), TYPE_STREAM_DEVICE, StreamDeviceClass))
#define STREAM_DEVICE_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS((obj), TYPE_STREAM_DEVICE, StreamDeviceClass))

typedef struct StreamDevice StreamDevice;
typedef struct StreamDeviceClass StreamDeviceClass;

struct StreamDevice {
    RedCharDevice parent;

    StreamDevHeader hdr;
    uint8_t hdr_pos;
    union {
        StreamMsgFormat format;
        StreamMsgCapabilities capabilities;
        StreamMsgCursorSet cursor_set;
        StreamMsgCursorMove cursor_move;
        uint8_t buf[STREAM_MSG_CAPABILITIES_MAX_BYTES];
    } *msg;
    uint32_t msg_pos;
    uint32_t msg_len;
    bool has_error;
    bool opened;
    bool flow_stopped;
    StreamChannel *stream_channel;
    CursorChannel *cursor_channel;
};

struct StreamDeviceClass {
    RedCharDeviceClass parent_class;
};

static GType stream_device_get_type(void) G_GNUC_CONST;
static StreamDevice *stream_device_new(SpiceCharDeviceInstance *sin, RedsState *reds);

G_DEFINE_TYPE(StreamDevice, stream_device, RED_TYPE_CHAR_DEVICE)

typedef bool StreamMsgHandler(StreamDevice *dev, SpiceCharDeviceInstance *sin)
    SPICE_GNUC_WARN_UNUSED_RESULT;

static StreamMsgHandler handle_msg_format, handle_msg_data, handle_msg_cursor_set,
    handle_msg_cursor_move;

static bool handle_msg_invalid(StreamDevice *dev, SpiceCharDeviceInstance *sin,
                               const char *error_msg) SPICE_GNUC_WARN_UNUSED_RESULT;

static bool
stream_device_partial_read(StreamDevice *dev, SpiceCharDeviceInstance *sin)
{
    SpiceCharDeviceInterface *sif;
    int n;
    bool handled = false;

    sif = spice_char_device_get_interface(sin);

    // in order to get in sync every time we open the device we need to discard data here.
    // Qemu keeps a buffer of data which is used only during spice_server_char_device_wakeup
    // from Qemu
    if (G_UNLIKELY(dev->has_error)) {
        uint8_t buf[16 * 1024];
        while (sif->read(sin, buf, sizeof(buf)) > 0) {
            continue;
        }
        return false;
    }

    if (dev->flow_stopped || !dev->stream_channel) {
        return false;
    }

    /* read header */
    while (dev->hdr_pos < sizeof(dev->hdr)) {
        n = sif->read(sin, (uint8_t *) &dev->hdr + dev->hdr_pos, sizeof(dev->hdr) - dev->hdr_pos);
        if (n <= 0) {
            return false;
        }
        dev->hdr_pos += n;
        if (dev->hdr_pos >= sizeof(dev->hdr)) {
            dev->hdr.type = GUINT16_FROM_LE(dev->hdr.type);
            dev->hdr.size = GUINT32_FROM_LE(dev->hdr.size);
            dev->msg_pos = 0;
        }
    }

    switch ((StreamMsgType) dev->hdr.type) {
    case STREAM_TYPE_FORMAT:
        if (dev->hdr.size != sizeof(StreamMsgFormat)) {
            handled = handle_msg_invalid(dev, sin, "Wrong size for StreamMsgFormat");
        } else {
            handled = handle_msg_format(dev, sin);
        }
        break;
    case STREAM_TYPE_DATA:
        handled = handle_msg_data(dev, sin);
        break;
    case STREAM_TYPE_CURSOR_SET:
        handled = handle_msg_cursor_set(dev, sin);
        break;
    case STREAM_TYPE_CURSOR_MOVE:
        if (dev->hdr.size != sizeof(StreamMsgCursorMove)) {
            handled = handle_msg_invalid(dev, sin, "Wrong size for StreamMsgCursorMove");
        } else {
            handled = handle_msg_cursor_move(dev, sin);
        }
        break;
    case STREAM_TYPE_CAPABILITIES:
        /* FIXME */
    default:
        handled = handle_msg_invalid(dev, sin, "Invalid message type");
        break;
    }

    /* current message has been handled, so reset state and get ready to parse
     * the next message */
    if (handled) {
        dev->hdr_pos = 0;

        // Reallocate message buffer to the minimum.
        // Currently the only message that requires resizing is the cursor shape,
        // which is not expected to be sent so often.
        if (dev->msg_len > sizeof(*dev->msg)) {
            dev->msg = g_realloc(dev->msg, sizeof(*dev->msg));
            dev->msg_len = sizeof(*dev->msg);
        }
    }

    if (handled || dev->has_error) {
        // Qemu put the device on blocking state if we don't read all data
        // so schedule another read.
        // We arrive here if we processed that entire message or we
        // got an error, try to read another message or discard the
        // wrong data
        return true;
    }

    return false;
}

static RedPipeItem *
stream_device_read_msg_from_dev(RedCharDevice *self, SpiceCharDeviceInstance *sin)
{
    StreamDevice *dev = STREAM_DEVICE(self);

    while (stream_device_partial_read(dev, sin)) {
        continue;
    }
    return NULL;
}

static bool
handle_msg_invalid(StreamDevice *dev, SpiceCharDeviceInstance *sin, const char *error_msg)
{
    static const char default_error_msg[] = "Protocol error";

    if (ENABLE_EXTRA_CHECKS) {
        spice_assert(dev->hdr_pos >= sizeof(StreamDevHeader));
    }

    if (!error_msg) {
        error_msg = default_error_msg;
    }

    int msg_size = sizeof(StreamMsgNotifyError) + strlen(error_msg) + 1;
    int total_size = sizeof(StreamDevHeader) + msg_size;

    RedCharDevice *char_dev = RED_CHAR_DEVICE(dev);
    RedCharDeviceWriteBuffer *buf =
        red_char_device_write_buffer_get_server_no_token(char_dev, total_size);
    buf->buf_used = total_size;

    StreamDevHeader *const hdr = (StreamDevHeader *)buf->buf;
    hdr->protocol_version = STREAM_DEVICE_PROTOCOL;
    hdr->padding = 0;
    hdr->type = GUINT16_TO_LE(STREAM_TYPE_NOTIFY_ERROR);
    hdr->size = GUINT32_TO_LE(msg_size);

    StreamMsgNotifyError *const error = (StreamMsgNotifyError *)(hdr+1);
    error->error_code = GUINT32_TO_LE(0);
    strcpy((char *) error->msg, error_msg);

    red_char_device_write_buffer_add(char_dev, buf);

    dev->has_error = true;
    return false;
}

static bool
handle_msg_format(StreamDevice *dev, SpiceCharDeviceInstance *sin)
{
    SpiceCharDeviceInterface *sif = spice_char_device_get_interface(sin);

    if (ENABLE_EXTRA_CHECKS) {
        spice_assert(dev->hdr_pos >= sizeof(StreamDevHeader));
        spice_assert(dev->hdr.type == STREAM_TYPE_FORMAT);
    }

    int n = sif->read(sin, dev->msg->buf + dev->msg_pos, sizeof(StreamMsgFormat) - dev->msg_pos);
    if (n < 0) {
        return handle_msg_invalid(dev, sin, NULL);
    }

    dev->msg_pos += n;

    if (dev->msg_pos < sizeof(StreamMsgFormat)) {
        return false;
    }

    dev->msg->format.width = GUINT32_FROM_LE(dev->msg->format.width);
    dev->msg->format.height = GUINT32_FROM_LE(dev->msg->format.height);
    stream_channel_change_format(dev->stream_channel, &dev->msg->format);
    return true;
}

static bool
handle_msg_data(StreamDevice *dev, SpiceCharDeviceInstance *sin)
{
    SpiceCharDeviceInterface *sif = spice_char_device_get_interface(sin);
    int n;

    if (ENABLE_EXTRA_CHECKS) {
        spice_assert(dev->hdr_pos >= sizeof(StreamDevHeader));
        spice_assert(dev->hdr.type == STREAM_TYPE_DATA);
    }

    while (1) {
        uint8_t buf[16 * 1024];
        n = sif->read(sin, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        // TODO collect all message ??
        // up: we send a single frame together
        // down: guest can cause a crash
        stream_channel_send_data(dev->stream_channel, buf, n, reds_get_mm_time());
        dev->hdr.size -= n;
    }

    return dev->hdr.size == 0;
}

/*
 * Returns number of bits required for a pixel of a given cursor type.
 *
 * Take into account mask bits.
 * Returns 0 on error.
 */
static unsigned int
get_cursor_type_bits(unsigned int cursor_type)
{
    switch (cursor_type) {
    case SPICE_CURSOR_TYPE_ALPHA:
        // RGBA
        return 32;
    case SPICE_CURSOR_TYPE_COLOR24:
        // RGB + bitmask
        return 24 + 1;
    case SPICE_CURSOR_TYPE_COLOR32:
        // RGBx + bitmask
        return 32 + 1;
    default:
        return 0;
    }
}

static RedCursorCmd *
stream_msg_cursor_set_to_cursor_cmd(const StreamMsgCursorSet *msg, size_t msg_size)
{
    RedCursorCmd *cmd = g_new0(RedCursorCmd, 1);
    cmd->type = QXL_CURSOR_SET;
    cmd->u.set.position.x = 0; // TODO
    cmd->u.set.position.y = 0; // TODO
    cmd->u.set.visible = 1; // TODO
    SpiceCursor *cursor = &cmd->u.set.shape;
    cursor->header.unique = 0;
    cursor->header.type = msg->type;
    cursor->header.width = GUINT16_FROM_LE(msg->width);
    cursor->header.height = GUINT16_FROM_LE(msg->height);
    cursor->header.hot_spot_x = GUINT16_FROM_LE(msg->hot_spot_x);
    cursor->header.hot_spot_y = GUINT16_FROM_LE(msg->hot_spot_y);

    /* Limit cursor size to prevent DoS */
    if (cursor->header.width > STREAM_MSG_CURSOR_SET_MAX_WIDTH ||
        cursor->header.height > STREAM_MSG_CURSOR_SET_MAX_HEIGHT) {
        g_free(cmd);
        return NULL;
    }

    const unsigned int cursor_bits = get_cursor_type_bits(cursor->header.type);
    if (cursor_bits == 0) {
        g_free(cmd);
        return NULL;
    }

    /* Check that enough data has been sent for the cursor.
     * Note that these computations can't overflow due to sizes checks
     * above. */
    size_t size_required = cursor->header.width * cursor->header.height;
    size_required = SPICE_ALIGN(size_required * cursor_bits, 8) / 8u;
    if (msg_size < sizeof(StreamMsgCursorSet) + size_required) {
        g_free(cmd);
        return NULL;
    }
    cursor->data_size = size_required;
    cursor->data = spice_memdup(msg->data, size_required);
    return cmd;
}

static bool
handle_msg_cursor_set(StreamDevice *dev, SpiceCharDeviceInstance *sin)
{
    // Calculate the maximum size required to send the pixel data for a cursor that is the
    // maximum size using the format that requires the largest number of bits per pixel
    // (SPICE_CURSOR_TYPE_COLOR32 requires 33 bits per pixel, see get_cursor_type_bits())
    const unsigned int max_cursor_set_size =
        sizeof(StreamMsgCursorSet) +
        (STREAM_MSG_CURSOR_SET_MAX_WIDTH * 4 + (STREAM_MSG_CURSOR_SET_MAX_WIDTH + 7)/8)
        * STREAM_MSG_CURSOR_SET_MAX_HEIGHT;

    SpiceCharDeviceInterface *sif = spice_char_device_get_interface(sin);

    if (dev->hdr.size < sizeof(StreamMsgCursorSet) || dev->hdr.size > max_cursor_set_size) {
        // we could skip the message but on the other hand the guest
        // is buggy in any case
        return handle_msg_invalid(dev, sin, "Cursor size is invalid");
    }

    // read part of the message till we get all
    if (dev->msg_len < dev->hdr.size) {
        dev->msg = g_realloc(dev->msg, dev->hdr.size);
        dev->msg_len = dev->hdr.size;
    }
    int n = sif->read(sin, dev->msg->buf + dev->msg_pos, dev->hdr.size - dev->msg_pos);
    if (n <= 0) {
        return false;
    }
    dev->msg_pos += n;
    if (dev->msg_pos != dev->hdr.size) {
        return false;
    }

    // transform the message to a cursor command and process it
    RedCursorCmd *cmd = stream_msg_cursor_set_to_cursor_cmd(&dev->msg->cursor_set, dev->msg_pos);
    if (!cmd) {
        return handle_msg_invalid(dev, sin, NULL);
    }
    cursor_channel_process_cmd(dev->cursor_channel, cmd);

    return true;
}

static bool
handle_msg_cursor_move(StreamDevice *dev, SpiceCharDeviceInstance *sin)
{
    SpiceCharDeviceInterface *sif = spice_char_device_get_interface(sin);
    int n = sif->read(sin, dev->msg->buf + dev->msg_pos, dev->hdr.size - dev->msg_pos);
    if (n <= 0) {
        return false;
    }
    dev->msg_pos += n;
    if (dev->msg_pos != dev->hdr.size) {
        return false;
    }

    StreamMsgCursorMove *move = &dev->msg->cursor_move;
    move->x = GINT32_FROM_LE(move->x);
    move->y = GINT32_FROM_LE(move->y);

    RedCursorCmd *cmd = g_new0(RedCursorCmd, 1);
    cmd->type = QXL_CURSOR_MOVE;
    cmd->u.position.x = move->x;
    cmd->u.position.y = move->y;

    cursor_channel_process_cmd(dev->cursor_channel, cmd);

    return true;
}

static void
stream_device_send_msg_to_client(RedCharDevice *self, RedPipeItem *msg, RedClient *client)
{
}

static void
stream_device_send_tokens_to_client(RedCharDevice *self, RedClient *client, uint32_t tokens)
{
    spice_printerr("Not implemented!");
}

static void
stream_device_remove_client(RedCharDevice *self, RedClient *client)
{
}

static void
stream_device_stream_start(void *opaque, StreamMsgStartStop *start,
                           StreamChannel *stream_channel G_GNUC_UNUSED)
{
    StreamDevice *dev = (StreamDevice *) opaque;

    if (!dev->opened) {
        return;
    }

    int msg_size = sizeof(*start) + sizeof(start->codecs[0]) * start->num_codecs;
    int total_size = sizeof(StreamDevHeader) + msg_size;

    RedCharDevice *char_dev = RED_CHAR_DEVICE(dev);
    RedCharDeviceWriteBuffer *buf =
        red_char_device_write_buffer_get_server_no_token(char_dev, total_size);
    buf->buf_used = total_size;

    StreamDevHeader *hdr = (StreamDevHeader *)buf->buf;
    hdr->protocol_version = STREAM_DEVICE_PROTOCOL;
    hdr->padding = 0;
    hdr->type = GUINT16_TO_LE(STREAM_TYPE_START_STOP);
    hdr->size = GUINT32_TO_LE(msg_size);

    memcpy(&hdr[1], start, msg_size);

    red_char_device_write_buffer_add(char_dev, buf);
}

static void
stream_device_stream_queue_stat(void *opaque, const StreamQueueStat *stats G_GNUC_UNUSED,
                                StreamChannel *stream_channel G_GNUC_UNUSED)
{
    StreamDevice *dev = (StreamDevice *) opaque;

    if (!dev->opened) {
        return;
    }

    // very easy control flow... if any data stop
    // this seems a very small queue but as we use tcp
    // there's already that queue
    if (stats->num_items) {
        dev->flow_stopped = true;
        return;
    }

    if (dev->flow_stopped) {
        dev->flow_stopped = false;
        // TODO resume flow...
        // avoid recursion if we need to call get data from data handling from
        // data handling
        red_char_device_wakeup(&dev->parent);
    }
}

RedCharDevice *
stream_device_connect(RedsState *reds, SpiceCharDeviceInstance *sin)
{
    SpiceCharDeviceInterface *sif;

    StreamDevice *dev = stream_device_new(sin, reds);

    sif = spice_char_device_get_interface(sin);
    if (sif->state) {
        sif->state(sin, 1);
    }

    return RED_CHAR_DEVICE(dev);
}

static void
stream_device_dispose(GObject *object)
{
    StreamDevice *dev = STREAM_DEVICE(object);

    if (dev->stream_channel) {
        // close all current connections and drop the reference
        red_channel_destroy(RED_CHANNEL(dev->stream_channel));
        dev->stream_channel = NULL;
    }
    if (dev->cursor_channel) {
        // close all current connections and drop the reference
        red_channel_destroy(RED_CHANNEL(dev->cursor_channel));
        dev->cursor_channel = NULL;
    }
}

static void
stream_device_finalize(GObject *object)
{
    StreamDevice *dev = STREAM_DEVICE(object);

    g_free(dev->msg);
    dev->msg = NULL;
    dev->msg_len = 0;
    dev->msg_pos = 0;
}

static void
allocate_channels(StreamDevice *dev)
{
    if (dev->stream_channel) {
        return;
    }

    SpiceServer* reds = red_char_device_get_server(RED_CHAR_DEVICE(dev));
    SpiceCoreInterfaceInternal* core = reds_get_core_interface(reds);

    int id = reds_get_free_channel_id(reds, SPICE_CHANNEL_DISPLAY);
    g_return_if_fail(id >= 0);

    StreamChannel *stream_channel = stream_channel_new(reds, id);

    CursorChannel *cursor_channel = cursor_channel_new(reds, id, core);
    ClientCbs client_cbs = { NULL, };
    client_cbs.connect = (channel_client_connect_proc) cursor_channel_connect;
    client_cbs.migrate = cursor_channel_client_migrate;
    red_channel_register_client_cbs(RED_CHANNEL(cursor_channel), &client_cbs, NULL);
    reds_register_channel(reds, RED_CHANNEL(cursor_channel));

    dev->stream_channel = stream_channel;
    dev->cursor_channel = cursor_channel;

    stream_channel_register_start_cb(stream_channel, stream_device_stream_start, dev);
    stream_channel_register_queue_stat_cb(stream_channel, stream_device_stream_queue_stat, dev);
}

static void
reset_channels(StreamDevice *dev)
{
    if (dev->stream_channel) {
        stream_channel_reset(dev->stream_channel);
    }
}

static void
stream_device_port_event(RedCharDevice *char_dev, uint8_t event)
{
    if (event != SPICE_PORT_EVENT_OPENED && event != SPICE_PORT_EVENT_CLOSED) {
        return;
    }

    StreamDevice *dev = STREAM_DEVICE(char_dev);

    // reset device and channel on close/open
    dev->opened = (event == SPICE_PORT_EVENT_OPENED);
    if (dev->opened) {
        allocate_channels(dev);
    }
    dev->hdr_pos = 0;
    dev->msg_pos = 0;
    dev->has_error = false;
    dev->flow_stopped = false;
    red_char_device_reset(char_dev);
    reset_channels(dev);
}

static void
stream_device_class_init(StreamDeviceClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    RedCharDeviceClass *char_dev_class = RED_CHAR_DEVICE_CLASS(klass);

    object_class->dispose = stream_device_dispose;
    object_class->finalize = stream_device_finalize;

    char_dev_class->read_one_msg_from_device = stream_device_read_msg_from_dev;
    char_dev_class->send_msg_to_client = stream_device_send_msg_to_client;
    char_dev_class->send_tokens_to_client = stream_device_send_tokens_to_client;
    char_dev_class->remove_client = stream_device_remove_client;
    char_dev_class->port_event = stream_device_port_event;
}

static void
stream_device_init(StreamDevice *dev)
{
    dev->msg = g_malloc(sizeof(*dev->msg));
    dev->msg_len = sizeof(*dev->msg);
}

static StreamDevice *
stream_device_new(SpiceCharDeviceInstance *sin, RedsState *reds)
{
    return g_object_new(TYPE_STREAM_DEVICE,
                        "sin", sin,
                        "spice-server", reds,
                        "client-tokens-interval", 0ULL,
                        "self-tokens", ~0ULL,
                        NULL);
}
