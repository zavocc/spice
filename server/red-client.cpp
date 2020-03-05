/*
    Copyright (C) 2009-2016 Red Hat, Inc.

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
#include <config.h>

#include "red-channel.h"
#include "red-client.h"
#include "reds.h"

#define FOREACH_CHANNEL_CLIENT(_client, _data) \
    GLIST_FOREACH(_client->channels, RedChannelClient, _data)

struct RedClientClass
{
    GObjectClass parent_class;
};

G_DEFINE_TYPE(RedClient, red_client, G_TYPE_OBJECT)

enum {
    PROP0,
    PROP_SPICE_SERVER,
    PROP_MIGRATED
};

static void
red_client_get_property (GObject    *object,
                         guint       property_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
    RedClient *self = RED_CLIENT(object);

    switch (property_id)
    {
        case PROP_SPICE_SERVER:
            g_value_set_pointer(value, self->reds);
            break;
        case PROP_MIGRATED:
            g_value_set_boolean(value, self->during_target_migrate);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
red_client_set_property (GObject      *object,
                         guint         property_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
    RedClient *self = RED_CLIENT(object);

    switch (property_id)
    {
        case PROP_SPICE_SERVER:
            self->reds = (RedsState*) g_value_get_pointer(value);
            break;
        case PROP_MIGRATED:
            self->during_target_migrate = g_value_get_boolean(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
red_client_finalize (GObject *object)
{
    RedClient *self = RED_CLIENT(object);

    if (self->mcc) {
        self->mcc->unref();
        self->mcc = nullptr;
    }
    spice_debug("release client=%p", self);
    pthread_mutex_destroy(&self->lock);

    G_OBJECT_CLASS (red_client_parent_class)->finalize (object);
}

static void
red_client_class_init (RedClientClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = red_client_get_property;
  object_class->set_property = red_client_set_property;
  object_class->finalize = red_client_finalize;

  g_object_class_install_property(object_class,
                                  PROP_SPICE_SERVER,
                                  g_param_spec_pointer("spice-server",
                                                       "Spice server",
                                                       "The Spice Server",
                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property(object_class,
                                  PROP_MIGRATED,
                                  g_param_spec_boolean("migrated",
                                                       "migrated",
                                                       "Whether this client was migrated",
                                                       FALSE,
                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
red_client_init(RedClient *self)
{
    pthread_mutex_init(&self->lock, NULL);
    self->thread_id = pthread_self();
}

RedClient *red_client_new(RedsState *reds, int migrated)
{
    return (RedClient*) g_object_new(RED_TYPE_CLIENT,
                        "spice-server", reds,
                        "migrated", migrated,
                        NULL);
}

void RedClient::set_migration_seamless() // dest
{
    RedChannelClient *rcc;

    spice_assert(during_target_migrate);
    pthread_mutex_lock(&lock);
    seamless_migrate = TRUE;
    /* update channel clients that got connected before the migration
     * type was set. red_client_add_channel will handle newer channel clients */
    FOREACH_CHANNEL_CLIENT(this, rcc) {
        if (rcc->set_migration_seamless()) {
            num_migrated_channels++;
        }
    }
    pthread_mutex_unlock(&lock);
}

void RedClient::migrate()
{
    RedChannelClient *rcc;
    RedChannel *channel;

    if (!pthread_equal(pthread_self(), thread_id)) {
        spice_warning("client->thread_id (%p) != "
                      "pthread_self (%p)."
                      "If one of the threads is != io-thread && != vcpu-thread,"
                      " this might be a BUG",
                      (void*) thread_id, (void*) pthread_self());
    }
    FOREACH_CHANNEL_CLIENT(this, rcc) {
        if (rcc->is_connected()) {
            channel = rcc->get_channel();
            channel->migrate_client(rcc);
        }
    }
}

void RedClient::destroy()
{
    if (!pthread_equal(pthread_self(), thread_id)) {
        spice_warning("client->thread_id (%p) != "
                      "pthread_self (%p)."
                      "If one of the threads is != io-thread && != vcpu-thread,"
                      " this might be a BUG",
                      (void*) thread_id,
                      (void*) pthread_self());
    }

    pthread_mutex_lock(&lock);
    spice_debug("destroy this %p with #channels=%d", this, g_list_length(channels));
    // This makes sure that we won't try to add new RedChannelClient instances
    // to the RedClient::channels list while iterating it
    disconnecting = TRUE;
    while (channels) {
        RedChannel *channel;
        RedChannelClient *rcc = (RedChannelClient *) channels->data;

        // Remove the RedChannelClient we are processing from the list
        // Note that we own the object so it is safe to do some operations on it.
        // This manual scan of the list is done to have a thread safe
        // iteration of the list
        channels = g_list_delete_link(channels, channels);

        // prevent dead lock disconnecting rcc (which can happen
        // in the same thread or synchronously on another one)
        pthread_mutex_unlock(&lock);

        // some channels may be in other threads, so disconnection
        // is not synchronous.
        channel = rcc->get_channel();

        // some channels may be in other threads. However we currently
        // assume disconnect is synchronous (we changed the dispatcher
        // to wait for disconnection)
        // TODO: should we go back to async. For this we need to use
        // ref count for channel clients.
        channel->disconnect_client(rcc);

        spice_assert(rcc->pipe_is_empty());
        spice_assert(rcc->no_item_being_sent());

        rcc->unref();
        pthread_mutex_lock(&lock);
    }
    pthread_mutex_unlock(&lock);
    unref();
}


/* client->lock should be locked */
static RedChannelClient *red_client_get_channel(RedClient *client, int type, int id)
{
    RedChannelClient *rcc;

    FOREACH_CHANNEL_CLIENT(client, rcc) {
        RedChannel *channel;

        channel = rcc->get_channel();
        if (channel->type() == type && channel->id() == id) {
            return rcc;
        }
    }
    return NULL;
}

gboolean RedClient::add_channel(RedChannelClient *rcc, GError **error)
{
    RedChannel *channel;
    gboolean result = TRUE;

    spice_assert(rcc);
    channel = rcc->get_channel();

    pthread_mutex_lock(&lock);

    uint32_t type = channel->type();
    uint32_t id = channel->id();
    if (disconnecting) {
        g_set_error(error,
                    SPICE_SERVER_ERROR,
                    SPICE_SERVER_ERROR_FAILED,
                    "Client %p got disconnected while connecting channel type %d id %d",
                    this, type, id);
        result = FALSE;
        goto cleanup;
    }

    if (red_client_get_channel(this, type, id)) {
        g_set_error(error,
                    SPICE_SERVER_ERROR,
                    SPICE_SERVER_ERROR_FAILED,
                    "Client %p: duplicate channel type %d id %d",
                    this, type, id);
        result = FALSE;
        goto cleanup;
    }

    // first must be the main one
    if (!mcc) {
        // FIXME use dynamic_cast to check type
        // spice_assert(MAIN_CHANNEL_CLIENT(rcc) != NULL);
        rcc->ref();
        mcc = (MainChannelClient *) rcc;
    }
    channels = g_list_prepend(channels, rcc);
    if (during_target_migrate && seamless_migrate) {
        if (rcc->set_migration_seamless()) {
            num_migrated_channels++;
        }
    }

cleanup:
    pthread_mutex_unlock(&lock);
    return result;
}

MainChannelClient *RedClient::get_main()
{
    return mcc;
}

void RedClient::semi_seamless_migrate_complete()
{
    RedChannelClient *rcc;

    pthread_mutex_lock(&lock);
    if (!during_target_migrate || seamless_migrate) {
        spice_error("unexpected");
        pthread_mutex_unlock(&lock);
        return;
    }
    during_target_migrate = FALSE;
    FOREACH_CHANNEL_CLIENT(this, rcc) {
        rcc->semi_seamless_migration_complete();
    }
    pthread_mutex_unlock(&lock);
    reds_on_client_semi_seamless_migrate_complete(reds, this);
}

/* should be called only from the main thread */
int RedClient::during_migrate_at_target()
{
    int ret;
    pthread_mutex_lock(&lock);
    ret = during_target_migrate;
    pthread_mutex_unlock(&lock);
    return ret;
}

void RedClient::remove_channel(RedChannelClient *rcc)
{
    RedClient *client = rcc->get_client();
    pthread_mutex_lock(&client->lock);
    GList *link = g_list_find(client->channels, rcc);
    if (link) {
        client->channels = g_list_delete_link(client->channels, link);
    }
    pthread_mutex_unlock(&client->lock);
    if (link) {
        rcc->unref();
    }
}

/* returns TRUE If all channels are finished migrating, FALSE otherwise */
gboolean RedClient::seamless_migration_done_for_channel()
{
    gboolean ret = FALSE;

    pthread_mutex_lock(&lock);
    num_migrated_channels--;
    /* we assume we always have at least one channel who has migration data transfer,
     * otherwise, this flag will never be set back to FALSE*/
    if (!num_migrated_channels) {
        during_target_migrate = FALSE;
        seamless_migrate = FALSE;
        /* migration completion might have been triggered from a different thread
         * than the main thread */
        reds_get_main_dispatcher(reds)->seamless_migrate_dst_complete(this);
        ret = TRUE;
    }
    pthread_mutex_unlock(&lock);

    return ret;
}

gboolean RedClient::is_disconnecting()
{
    gboolean ret;
    pthread_mutex_lock(&lock);
    ret =  disconnecting;
    pthread_mutex_unlock(&lock);
    return ret;
}

void RedClient::set_disconnecting()
{
    pthread_mutex_lock(&lock);
    disconnecting = TRUE;
    pthread_mutex_unlock(&lock);
}

RedsState *RedClient::get_server()
{
    return reds;
}
