/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009 Red Hat, Inc.

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

#include <common/ring.h>

#include "red-common.h"
#include "reds.h"
#include "red-channel-client.h"
#include "main-channel.h"
#include "main-channel-client.h"

int main_channel_is_connected(MainChannel *main_chan)
{
    return red_channel_is_connected(&main_chan->base);
}

/*
 * When the main channel is disconnected, disconnect the entire client.
 */
static void main_channel_client_on_disconnect(RedChannelClient *rcc)
{
    RedsState *reds = red_channel_get_server(red_channel_client_get_channel(rcc));
    spice_printerr("rcc=%p", rcc);
    main_dispatcher_client_disconnect(reds_get_main_dispatcher(reds),
                                      red_channel_client_get_client(rcc));
}

RedClient *main_channel_get_client_by_link_id(MainChannel *main_chan, uint32_t connection_id)
{
    GListIter iter;
    RedChannelClient *rcc;

    FOREACH_CLIENT(main_chan, iter, rcc) {
        MainChannelClient *mcc = MAIN_CHANNEL_CLIENT(rcc);
        if (main_channel_client_get_connection_id(mcc) == connection_id) {
            return red_channel_client_get_client(rcc);
        }
    }
    return NULL;
}

static void main_channel_push_channels(MainChannelClient *mcc)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(mcc);
    if (red_client_during_migrate_at_target(red_channel_client_get_client(rcc))) {
        spice_printerr("warning: ignoring unexpected SPICE_MSGC_MAIN_ATTACH_CHANNELS"
                   "during migration");
        return;
    }
    red_channel_client_pipe_add_type(RED_CHANNEL_CLIENT(mcc), RED_PIPE_ITEM_TYPE_MAIN_CHANNELS_LIST);
}

void main_channel_push_mouse_mode(MainChannel *main_chan, int current_mode,
                                  int is_client_mouse_allowed)
{
    MainMouseModeItemInfo info = {
        .current_mode=current_mode,
        .is_client_mouse_allowed=is_client_mouse_allowed,
    };

    red_channel_pipes_new_add_push(&main_chan->base,
        main_mouse_mode_item_new, &info);
}

void main_channel_push_agent_connected(MainChannel *main_chan)
{
    if (red_channel_test_remote_cap(&main_chan->base, SPICE_MAIN_CAP_AGENT_CONNECTED_TOKENS)) {
        red_channel_pipes_add_type(&main_chan->base, RED_PIPE_ITEM_TYPE_MAIN_AGENT_CONNECTED_TOKENS);
    } else {
        red_channel_pipes_add_empty_msg(&main_chan->base, SPICE_MSG_MAIN_AGENT_CONNECTED);
    }
}

void main_channel_push_agent_disconnected(MainChannel *main_chan)
{
    red_channel_pipes_add_type(&main_chan->base, RED_PIPE_ITEM_TYPE_MAIN_AGENT_DISCONNECTED);
}

static void main_channel_push_migrate_data_item(MainChannel *main_chan)
{
    red_channel_pipes_add_type(&main_chan->base, RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_DATA);
}

static int main_channel_handle_migrate_data(RedChannelClient *rcc,
    uint32_t size, void *message)
{
    RedChannel *channel = red_channel_client_get_channel(rcc);
    MainChannelClient *mcc = MAIN_CHANNEL_CLIENT(rcc);
    SpiceMigrateDataHeader *header = (SpiceMigrateDataHeader *)message;

    /* not supported with multi-clients */
    spice_assert(g_list_length(channel->clients) == 1);

    if (size < sizeof(SpiceMigrateDataHeader) + sizeof(SpiceMigrateDataMain)) {
        spice_printerr("bad message size %u", size);
        return FALSE;
    }
    if (!migration_protocol_validate_header(header,
                                            SPICE_MIGRATE_DATA_MAIN_MAGIC,
                                            SPICE_MIGRATE_DATA_MAIN_VERSION)) {
        spice_error("bad header");
        return FALSE;
    }
    return reds_handle_migrate_data(channel->reds, mcc, (SpiceMigrateDataMain *)(header + 1), size);
}

void main_channel_push_multi_media_time(MainChannel *main_chan, int time)
{
    MainMultiMediaTimeItemInfo info = {
        .time = time,
    };

    red_channel_pipes_new_add_push(&main_chan->base,
        main_multi_media_time_item_new, &info);
}

static void main_channel_fill_mig_target(MainChannel *main_channel, RedsMigSpice *mig_target)
{
    spice_assert(mig_target);
    free(main_channel->mig_target.host);
    main_channel->mig_target.host = spice_strdup(mig_target->host);
    free(main_channel->mig_target.cert_subject);
    if (mig_target->cert_subject) {
        main_channel->mig_target.cert_subject = spice_strdup(mig_target->cert_subject);
    } else {
        main_channel->mig_target.cert_subject = NULL;
    }
    main_channel->mig_target.port = mig_target->port;
    main_channel->mig_target.sport = mig_target->sport;
}

void main_channel_migrate_switch(MainChannel *main_chan, RedsMigSpice *mig_target)
{
    main_channel_fill_mig_target(main_chan, mig_target);
    red_channel_pipes_add_type(&main_chan->base, RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_SWITCH_HOST);
}

static int main_channel_handle_parsed(RedChannelClient *rcc, uint32_t size, uint16_t type,
                                      void *message)
{
    RedChannel *channel = red_channel_client_get_channel(rcc);
    MainChannel *main_chan = SPICE_CONTAINEROF(channel, MainChannel, base);
    MainChannelClient *mcc = MAIN_CHANNEL_CLIENT(rcc);

    switch (type) {
    case SPICE_MSGC_MAIN_AGENT_START: {
        SpiceMsgcMainAgentStart *tokens;

        spice_printerr("agent start");
        if (!main_chan) {
            return FALSE;
        }
        tokens = (SpiceMsgcMainAgentStart *)message;
        reds_on_main_agent_start(channel->reds, mcc, tokens->num_tokens);
        break;
    }
    case SPICE_MSGC_MAIN_AGENT_DATA: {
        reds_on_main_agent_data(channel->reds, mcc, message, size);
        break;
    }
    case SPICE_MSGC_MAIN_AGENT_TOKEN: {
        SpiceMsgcMainAgentTokens *tokens;

        tokens = (SpiceMsgcMainAgentTokens *)message;
        reds_on_main_agent_tokens(channel->reds, mcc, tokens->num_tokens);
        break;
    }
    case SPICE_MSGC_MAIN_ATTACH_CHANNELS:
        main_channel_push_channels(mcc);
        break;
    case SPICE_MSGC_MAIN_MIGRATE_CONNECTED:
        main_channel_client_handle_migrate_connected(mcc,
                                                     TRUE /* success */,
                                                     FALSE /* seamless */);
        break;
    case SPICE_MSGC_MAIN_MIGRATE_CONNECTED_SEAMLESS:
        main_channel_client_handle_migrate_connected(mcc,
                                                     TRUE /* success */,
                                                     TRUE /* seamless */);
        break;
    case SPICE_MSGC_MAIN_MIGRATE_CONNECT_ERROR:
        main_channel_client_handle_migrate_connected(mcc, FALSE, FALSE);
        break;
    case SPICE_MSGC_MAIN_MIGRATE_DST_DO_SEAMLESS:
        main_channel_client_handle_migrate_dst_do_seamless(mcc,
            ((SpiceMsgcMainMigrateDstDoSeamless *)message)->src_version);
        break;
    case SPICE_MSGC_MAIN_MOUSE_MODE_REQUEST:
        reds_on_main_mouse_mode_request(channel->reds, message, size);
        break;
    case SPICE_MSGC_PONG: {
        main_channel_client_handle_pong(mcc, (SpiceMsgPing *)message, size);
        break;
    }
    case SPICE_MSGC_DISCONNECTING:
        break;
    case SPICE_MSGC_MAIN_MIGRATE_END:
        main_channel_client_handle_migrate_end(mcc);
        break;
    default:
        return red_channel_client_handle_message(rcc, size, type, message);
    }
    return TRUE;
}

static uint8_t *main_channel_alloc_msg_rcv_buf(RedChannelClient *rcc,
                                               uint16_t type,
                                               uint32_t size)
{
    RedChannel *channel = red_channel_client_get_channel(rcc);
    MainChannel *main_chan = SPICE_CONTAINEROF(channel, MainChannel, base);
    MainChannelClient *mcc = MAIN_CHANNEL_CLIENT(rcc);

    if (type == SPICE_MSGC_MAIN_AGENT_DATA) {
        return reds_get_agent_data_buffer(channel->reds, mcc, size);
    } else {
        return main_chan->recv_buf;
    }
}

static void main_channel_release_msg_rcv_buf(RedChannelClient *rcc,
                                               uint16_t type,
                                               uint32_t size,
                                               uint8_t *msg)
{
    RedChannel *channel = red_channel_client_get_channel(rcc);
    if (type == SPICE_MSGC_MAIN_AGENT_DATA) {
        reds_release_agent_data_buffer(channel->reds, msg);
    }
}

static int main_channel_config_socket(RedChannelClient *rcc)
{
    return TRUE;
}

static int main_channel_handle_migrate_flush_mark(RedChannelClient *rcc)
{
    RedChannel *channel = red_channel_client_get_channel(rcc);
    spice_debug(NULL);
    main_channel_push_migrate_data_item(SPICE_CONTAINEROF(channel,
                                        MainChannel, base));
    return TRUE;
}

MainChannelClient *main_channel_link(MainChannel *channel, RedClient *client,
                                     RedsStream *stream, uint32_t connection_id, int migration,
                                     int num_common_caps, uint32_t *common_caps, int num_caps,
                                     uint32_t *caps)
{
    MainChannelClient *mcc;

    spice_assert(channel);

    // TODO - migration - I removed it from channel creation, now put it
    // into usage somewhere (not an issue until we return migration to it's
    // former glory)
    spice_printerr("add main channel client");
    mcc = main_channel_client_create(channel, client, stream, connection_id,
                                     num_common_caps, common_caps,
                                     num_caps, caps);
    return mcc;
}

int main_channel_getsockname(MainChannel *main_chan, struct sockaddr *sa, socklen_t *salen)
{
    return main_chan ? getsockname(red_channel_get_first_socket(&main_chan->base), sa, salen) : -1;
}

int main_channel_getpeername(MainChannel *main_chan, struct sockaddr *sa, socklen_t *salen)
{
    return main_chan ? getpeername(red_channel_get_first_socket(&main_chan->base), sa, salen) : -1;
}

// TODO: ? shouldn't it disonnect all clients? or shutdown all main_channels?
void main_channel_close(MainChannel *main_chan)
{
    int socketfd;

    if (main_chan && (socketfd = red_channel_get_first_socket(&main_chan->base)) != -1) {
        close(socketfd);
    }
}

MainChannel* main_channel_new(RedsState *reds)
{
    RedChannel *channel;
    ChannelCbs channel_cbs = { NULL, };
    ClientCbs client_cbs = {NULL, };

    channel_cbs.config_socket = main_channel_config_socket;
    channel_cbs.on_disconnect = main_channel_client_on_disconnect;
    channel_cbs.send_item = main_channel_client_send_item;
    channel_cbs.alloc_recv_buf = main_channel_alloc_msg_rcv_buf;
    channel_cbs.release_recv_buf = main_channel_release_msg_rcv_buf;
    channel_cbs.handle_migrate_flush_mark = main_channel_handle_migrate_flush_mark;
    channel_cbs.handle_migrate_data = main_channel_handle_migrate_data;

    // TODO: set the migration flag of the channel
    channel = red_channel_create_parser(sizeof(MainChannel), reds,
                                        reds_get_core_interface(reds),
                                        SPICE_CHANNEL_MAIN, 0,
                                        FALSE, /* handle_acks */
                                        spice_get_client_channel_parser(SPICE_CHANNEL_MAIN, NULL),
                                        main_channel_handle_parsed,
                                        &channel_cbs,
                                        SPICE_MIGRATE_NEED_FLUSH | SPICE_MIGRATE_NEED_DATA_TRANSFER);
    spice_assert(channel);
    red_channel_set_cap(channel, SPICE_MAIN_CAP_SEMI_SEAMLESS_MIGRATE);
    red_channel_set_cap(channel, SPICE_MAIN_CAP_SEAMLESS_MIGRATE);

    client_cbs.migrate = main_channel_client_migrate;
    red_channel_register_client_cbs(channel, &client_cbs, NULL);

    return MAIN_CHANNEL(channel);
}

static int main_channel_connect_semi_seamless(MainChannel *main_channel)
{
    GListIter iter;
    RedChannelClient *rcc;

    FOREACH_CLIENT(main_channel, iter, rcc) {
        MainChannelClient *mcc = MAIN_CHANNEL_CLIENT(rcc);
        if (main_channel_client_connect_semi_seamless(mcc))
            main_channel->num_clients_mig_wait++;
    }
    return main_channel->num_clients_mig_wait;
}

static int main_channel_connect_seamless(MainChannel *main_channel)
{
    GListIter iter;
    RedChannelClient *rcc;

    spice_assert(g_list_length(main_channel->base.clients) == 1);

    FOREACH_CLIENT(main_channel, iter, rcc) {
        MainChannelClient *mcc = MAIN_CHANNEL_CLIENT(rcc);
        main_channel_client_connect_seamless(mcc);
        main_channel->num_clients_mig_wait++;
    }
    return main_channel->num_clients_mig_wait;
}

int main_channel_migrate_connect(MainChannel *main_channel, RedsMigSpice *mig_target,
                                 int try_seamless)
{
    main_channel_fill_mig_target(main_channel, mig_target);
    main_channel->num_clients_mig_wait = 0;

    if (!main_channel_is_connected(main_channel)) {
        return 0;
    }

    if (!try_seamless) {
        return main_channel_connect_semi_seamless(main_channel);
    } else {
        RedChannelClient *rcc;

        rcc = g_list_nth_data(main_channel->base.clients, 0);

        if (!red_channel_client_test_remote_cap(rcc,
                                                SPICE_MAIN_CAP_SEAMLESS_MIGRATE)) {
            return main_channel_connect_semi_seamless(main_channel);
        } else {
            return main_channel_connect_seamless(main_channel);
        }
    }

}

void main_channel_migrate_cancel_wait(MainChannel *main_chan)
{
    GListIter iter;
    RedChannelClient *rcc;

    FOREACH_CLIENT(main_chan, iter, rcc) {
        MainChannelClient *mcc = MAIN_CHANNEL_CLIENT(rcc);
        main_channel_client_migrate_cancel_wait(mcc);
    }
    main_chan->num_clients_mig_wait = 0;
}

int main_channel_migrate_src_complete(MainChannel *main_chan, int success)
{
    GListIter iter;
    int semi_seamless_count = 0;
    RedChannelClient *rcc;

    spice_printerr("");

    if (!main_chan->base.clients) {
        spice_printerr("no peer connected");
        return 0;
    }

    FOREACH_CLIENT(main_chan, iter, rcc) {
        MainChannelClient *mcc = MAIN_CHANNEL_CLIENT(rcc);
        if (main_channel_client_migrate_src_complete(mcc, success))
            semi_seamless_count++;
   }
   return semi_seamless_count;
}

void main_channel_on_migrate_connected(MainChannel *main_channel,
                                       gboolean success, gboolean seamless)
{
        spice_assert(main_channel->num_clients_mig_wait);
        spice_assert(!seamless || main_channel->num_clients_mig_wait == 1);
        if (!--main_channel->num_clients_mig_wait) {
            reds_on_main_migrate_connected(red_channel_get_server(RED_CHANNEL(main_channel)),
                                           seamless && success);
        }
}

const RedsMigSpice* main_channel_get_migration_target(MainChannel *main_chan)
{
    return &main_chan->mig_target;
}
