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

#include <netinet/in.h> // IPPROTO_TCP
#include <netinet/tcp.h> // TCP_NODELAY
#include <fcntl.h>
#include <stddef.h> // NULL
#include <errno.h>
#include <spice/macros.h>
#include <spice/vd_agent.h>
#include <spice/protocol.h>
#include <stdbool.h>

#include "common/marshaller.h"
#include "common/messages.h"
#include "common/generated_server_marshallers.h"

#include "demarshallers.h"
#include "spice.h"
#include "red-common.h"
#include "reds.h"
#include "reds-stream.h"
#include "red-channel.h"
#include "main-channel.h"
#include "inputs-channel.h"
#include "migration-protocol.h"
#include "utils.h"

// TODO: RECEIVE_BUF_SIZE used to be the same for inputs_channel and main_channel
// since it was defined once in reds.c which contained both.
// Now that they are split we can give a more fitting value for inputs - what
// should it be?
#define REDS_AGENT_WINDOW_SIZE 10
#define REDS_NUM_INTERNAL_AGENT_MESSAGES 1

// approximate max receive message size
#define RECEIVE_BUF_SIZE \
    (4096 + (REDS_AGENT_WINDOW_SIZE + REDS_NUM_INTERNAL_AGENT_MESSAGES) * SPICE_AGENT_MAX_DATA_SIZE)

struct SpiceKbdState {
    bool push_ext;

    /* track key press state */
    bool key[0x7f];
    bool key_ext[0x7f];
    RedsState *reds;
};

static SpiceKbdState* spice_kbd_state_new(RedsState *reds)
{
    SpiceKbdState *st = spice_new0(SpiceKbdState, 1);
    st->reds = reds;
    return st;
}

RedsState* spice_kbd_state_get_server(SpiceKbdState *dev)
{
    return dev->reds;
}

struct SpiceMouseState {
    int dummy;
};

static SpiceMouseState* spice_mouse_state_new(void)
{
    return spice_new0(SpiceMouseState, 1);
}

struct SpiceTabletState {
    RedsState *reds;
};

static SpiceTabletState* spice_tablet_state_new(void)
{
    return spice_new0(SpiceTabletState, 1);
}

RedsState* spice_tablet_state_get_server(SpiceTabletState *st)
{
    return st->reds;
}

typedef struct InputsChannelClient {
    RedChannelClient base;
    uint16_t motion_count;
} InputsChannelClient;

struct InputsChannel {
    RedChannel base;
    uint8_t recv_buf[RECEIVE_BUF_SIZE];
    VDAgentMouseState mouse_state;
    int src_during_migrate;

    SpiceKbdInstance *keyboard;
    SpiceMouseInstance *mouse;
    SpiceTabletInstance *tablet;
};

enum {
    PIPE_ITEM_INPUTS_INIT = PIPE_ITEM_TYPE_CHANNEL_BASE,
    PIPE_ITEM_MOUSE_MOTION_ACK,
    PIPE_ITEM_KEY_MODIFIERS,
    PIPE_ITEM_MIGRATE_DATA,
};

typedef struct InputsPipeItem {
    PipeItem base;
} InputsPipeItem;

typedef struct KeyModifiersPipeItem {
    PipeItem base;
    uint8_t modifiers;
} KeyModifiersPipeItem;

typedef struct InputsInitPipeItem {
    PipeItem base;
    uint8_t modifiers;
} InputsInitPipeItem;

static SpiceTimer *key_modifiers_timer;


#define KEY_MODIFIERS_TTL (MSEC_PER_SEC * 2)

#define SCROLL_LOCK_SCAN_CODE 0x46
#define NUM_LOCK_SCAN_CODE 0x45
#define CAPS_LOCK_SCAN_CODE 0x3a

void inputs_channel_set_tablet_logical_size(InputsChannel *inputs, int x_res, int y_res)
{
    SpiceTabletInterface *sif;

    sif = SPICE_CONTAINEROF(inputs->tablet->base.sif, SpiceTabletInterface, base);
    sif->set_logical_size(inputs->tablet, x_res, y_res);
}

const VDAgentMouseState *inputs_channel_get_mouse_state(InputsChannel *inputs)
{
    return &inputs->mouse_state;
}

static uint8_t *inputs_channel_alloc_msg_rcv_buf(RedChannelClient *rcc,
                                                 uint16_t type,
                                                 uint32_t size)
{
    InputsChannel *inputs_channel = SPICE_CONTAINEROF(rcc->channel, InputsChannel, base);

    if (size > RECEIVE_BUF_SIZE) {
        spice_printerr("error: too large incoming message");
        return NULL;
    }
    return inputs_channel->recv_buf;
}

static void inputs_channel_release_msg_rcv_buf(RedChannelClient *rcc,
                                               uint16_t type,
                                               uint32_t size,
                                               uint8_t *msg)
{
}

#define OUTGOING_OK 0
#define OUTGOING_FAILED -1
#define OUTGOING_BLOCKED 1

#define RED_MOUSE_STATE_TO_LOCAL(state)     \
    ((state & SPICE_MOUSE_BUTTON_MASK_LEFT) |          \
     ((state & SPICE_MOUSE_BUTTON_MASK_MIDDLE) << 1) |   \
     ((state & SPICE_MOUSE_BUTTON_MASK_RIGHT) >> 1))

#define RED_MOUSE_BUTTON_STATE_TO_AGENT(state)                      \
    (((state & SPICE_MOUSE_BUTTON_MASK_LEFT) ? VD_AGENT_LBUTTON_MASK : 0) |    \
     ((state & SPICE_MOUSE_BUTTON_MASK_MIDDLE) ? VD_AGENT_MBUTTON_MASK : 0) |    \
     ((state & SPICE_MOUSE_BUTTON_MASK_RIGHT) ? VD_AGENT_RBUTTON_MASK : 0))

static void activate_modifiers_watch(RedsState *reds)
{
    reds_core_timer_start(reds, key_modifiers_timer, KEY_MODIFIERS_TTL);
}

static void kbd_push_scan(SpiceKbdInstance *sin, uint8_t scan)
{
    SpiceKbdInterface *sif;

    if (!sin) {
        return;
    }
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceKbdInterface, base);

    /* track XT scan code set 1 key state */
    if (scan == 0xe0) {
        sin->st->push_ext = TRUE;
    } else {
        bool *state = sin->st->push_ext ? sin->st->key : sin->st->key_ext;
        sin->st->push_ext = FALSE;
        state[scan & 0x7f] = !(scan & 0x80);
    }

    sif->push_scan_freg(sin, scan);
}

static uint8_t kbd_get_leds(SpiceKbdInstance *sin)
{
    SpiceKbdInterface *sif;

    if (!sin) {
        return 0;
    }
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceKbdInterface, base);
    return sif->get_leds(sin);
}

static PipeItem *inputs_key_modifiers_item_new(
    RedChannelClient *rcc, void *data, int num)
{
    KeyModifiersPipeItem *item = spice_malloc(sizeof(KeyModifiersPipeItem));

    pipe_item_init(&item->base, PIPE_ITEM_KEY_MODIFIERS);
    item->modifiers = *(uint8_t *)data;
    return &item->base;
}

static void inputs_channel_send_migrate_data(RedChannelClient *rcc,
                                             SpiceMarshaller *m,
                                             PipeItem *item)
{
    InputsChannelClient *icc = SPICE_CONTAINEROF(rcc, InputsChannelClient, base);
    InputsChannel *inputs = SPICE_CONTAINEROF(rcc->channel, InputsChannel, base);

    inputs->src_during_migrate = FALSE;
    red_channel_client_init_send_data(rcc, SPICE_MSG_MIGRATE_DATA, item);

    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_INPUTS_MAGIC);
    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_INPUTS_VERSION);
    spice_marshaller_add_uint16(m, icc->motion_count);
}

static void inputs_channel_release_pipe_item(RedChannelClient *rcc,
    PipeItem *base, int item_pushed)
{
    free(base);
}

static void inputs_channel_send_item(RedChannelClient *rcc, PipeItem *base)
{
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);

    switch (base->type) {
        case PIPE_ITEM_KEY_MODIFIERS:
        {
            SpiceMsgInputsKeyModifiers key_modifiers;

            red_channel_client_init_send_data(rcc, SPICE_MSG_INPUTS_KEY_MODIFIERS, base);
            key_modifiers.modifiers =
                SPICE_CONTAINEROF(base, KeyModifiersPipeItem, base)->modifiers;
            spice_marshall_msg_inputs_key_modifiers(m, &key_modifiers);
            break;
        }
        case PIPE_ITEM_INPUTS_INIT:
        {
            SpiceMsgInputsInit inputs_init;

            red_channel_client_init_send_data(rcc, SPICE_MSG_INPUTS_INIT, base);
            inputs_init.keyboard_modifiers =
                SPICE_CONTAINEROF(base, InputsInitPipeItem, base)->modifiers;
            spice_marshall_msg_inputs_init(m, &inputs_init);
            break;
        }
        case PIPE_ITEM_MOUSE_MOTION_ACK:
            red_channel_client_init_send_data(rcc, SPICE_MSG_INPUTS_MOUSE_MOTION_ACK, base);
            break;
        case PIPE_ITEM_MIGRATE_DATA:
            inputs_channel_send_migrate_data(rcc, m, base);
            break;
        default:
            spice_warning("invalid pipe iten %d", base->type);
            break;
    }
    red_channel_client_begin_send_message(rcc);
}

static int inputs_channel_handle_parsed(RedChannelClient *rcc, uint32_t size, uint16_t type,
                                        void *message)
{
    InputsChannel *inputs_channel = (InputsChannel *)rcc->channel;
    InputsChannelClient *icc = (InputsChannelClient *)rcc;
    uint32_t i;
    RedsState *reds = red_channel_get_server(&inputs_channel->base);

    switch (type) {
    case SPICE_MSGC_INPUTS_KEY_DOWN: {
        SpiceMsgcKeyDown *key_down = message;
        if (key_down->code == CAPS_LOCK_SCAN_CODE ||
            key_down->code == NUM_LOCK_SCAN_CODE ||
            key_down->code == SCROLL_LOCK_SCAN_CODE) {
            activate_modifiers_watch(reds);
        }
    }
    case SPICE_MSGC_INPUTS_KEY_UP: {
        SpiceMsgcKeyUp *key_up = message;
        for (i = 0; i < 4; i++) {
            uint8_t code = (key_up->code >> (i * 8)) & 0xff;
            if (code == 0) {
                break;
            }
            kbd_push_scan(inputs_channel_get_keyboard(inputs_channel), code);
        }
        break;
    }
    case SPICE_MSGC_INPUTS_KEY_SCANCODE: {
        uint8_t *code = message;
        for (i = 0; i < size; i++) {
            kbd_push_scan(inputs_channel_get_keyboard(inputs_channel), code[i]);
        }
        break;
    }
    case SPICE_MSGC_INPUTS_MOUSE_MOTION: {
        SpiceMouseInstance *mouse = inputs_channel_get_mouse(inputs_channel);
        SpiceMsgcMouseMotion *mouse_motion = message;

        if (++icc->motion_count % SPICE_INPUT_MOTION_ACK_BUNCH == 0 &&
            !inputs_channel->src_during_migrate) {
            red_channel_client_pipe_add_type(rcc, PIPE_ITEM_MOUSE_MOTION_ACK);
            icc->motion_count = 0;
        }
        if (mouse && reds_get_mouse_mode(reds) == SPICE_MOUSE_MODE_SERVER) {
            SpiceMouseInterface *sif;
            sif = SPICE_CONTAINEROF(mouse->base.sif, SpiceMouseInterface, base);
            sif->motion(mouse,
                        mouse_motion->dx, mouse_motion->dy, 0,
                        RED_MOUSE_STATE_TO_LOCAL(mouse_motion->buttons_state));
        }
        break;
    }
    case SPICE_MSGC_INPUTS_MOUSE_POSITION: {
        SpiceMsgcMousePosition *pos = message;
        SpiceTabletInstance *tablet = inputs_channel_get_tablet(inputs_channel);

        if (++icc->motion_count % SPICE_INPUT_MOTION_ACK_BUNCH == 0 &&
            !inputs_channel->src_during_migrate) {
            red_channel_client_pipe_add_type(rcc, PIPE_ITEM_MOUSE_MOTION_ACK);
            icc->motion_count = 0;
        }
        if (reds_get_mouse_mode(reds) != SPICE_MOUSE_MODE_CLIENT) {
            break;
        }
        spice_assert((reds_get_agent_mouse(reds) && reds_has_vdagent(reds)) || tablet);
        if (!reds_get_agent_mouse(reds) || !reds_has_vdagent(reds)) {
            SpiceTabletInterface *sif;
            sif = SPICE_CONTAINEROF(tablet->base.sif, SpiceTabletInterface, base);
            sif->position(tablet, pos->x, pos->y, RED_MOUSE_STATE_TO_LOCAL(pos->buttons_state));
            break;
        }
        VDAgentMouseState *mouse_state = &inputs_channel->mouse_state;
        mouse_state->x = pos->x;
        mouse_state->y = pos->y;
        mouse_state->buttons = RED_MOUSE_BUTTON_STATE_TO_AGENT(pos->buttons_state);
        mouse_state->display_id = pos->display_id;
        reds_handle_agent_mouse_event(reds, mouse_state);
        break;
    }
    case SPICE_MSGC_INPUTS_MOUSE_PRESS: {
        SpiceMsgcMousePress *mouse_press = message;
        int dz = 0;
        if (mouse_press->button == SPICE_MOUSE_BUTTON_UP) {
            dz = -1;
        } else if (mouse_press->button == SPICE_MOUSE_BUTTON_DOWN) {
            dz = 1;
        }
        if (reds_get_mouse_mode(reds) == SPICE_MOUSE_MODE_CLIENT) {
            if (reds_get_agent_mouse(reds) && reds_has_vdagent(reds)) {
                inputs_channel->mouse_state.buttons =
                    RED_MOUSE_BUTTON_STATE_TO_AGENT(mouse_press->buttons_state) |
                    (dz == -1 ? VD_AGENT_UBUTTON_MASK : 0) |
                    (dz == 1 ? VD_AGENT_DBUTTON_MASK : 0);
                reds_handle_agent_mouse_event(reds, &inputs_channel->mouse_state);
            } else if (inputs_channel_get_tablet(inputs_channel)) {
                SpiceTabletInterface *sif;
                sif = SPICE_CONTAINEROF(inputs_channel_get_tablet(inputs_channel)->base.sif, SpiceTabletInterface, base);
                sif->wheel(inputs_channel_get_tablet(inputs_channel), dz, RED_MOUSE_STATE_TO_LOCAL(mouse_press->buttons_state));
            }
        } else if (inputs_channel_get_mouse(inputs_channel)) {
            SpiceMouseInterface *sif;
            sif = SPICE_CONTAINEROF(inputs_channel_get_mouse(inputs_channel)->base.sif, SpiceMouseInterface, base);
            sif->motion(inputs_channel_get_mouse(inputs_channel), 0, 0, dz,
                        RED_MOUSE_STATE_TO_LOCAL(mouse_press->buttons_state));
        }
        break;
    }
    case SPICE_MSGC_INPUTS_MOUSE_RELEASE: {
        SpiceMsgcMouseRelease *mouse_release = message;
        if (reds_get_mouse_mode(reds) == SPICE_MOUSE_MODE_CLIENT) {
            if (reds_get_agent_mouse(reds) && reds_has_vdagent(reds)) {
                inputs_channel->mouse_state.buttons =
                    RED_MOUSE_BUTTON_STATE_TO_AGENT(mouse_release->buttons_state);
                reds_handle_agent_mouse_event(reds, &inputs_channel->mouse_state);
            } else if (inputs_channel_get_tablet(inputs_channel)) {
                SpiceTabletInterface *sif;
                sif = SPICE_CONTAINEROF(inputs_channel_get_tablet(inputs_channel)->base.sif, SpiceTabletInterface, base);
                sif->buttons(inputs_channel_get_tablet(inputs_channel), RED_MOUSE_STATE_TO_LOCAL(mouse_release->buttons_state));
            }
        } else if (inputs_channel_get_mouse(inputs_channel)) {
            SpiceMouseInterface *sif;
            sif = SPICE_CONTAINEROF(inputs_channel_get_mouse(inputs_channel)->base.sif, SpiceMouseInterface, base);
            sif->buttons(inputs_channel_get_mouse(inputs_channel),
                         RED_MOUSE_STATE_TO_LOCAL(mouse_release->buttons_state));
        }
        break;
    }
    case SPICE_MSGC_INPUTS_KEY_MODIFIERS: {
        SpiceMsgcKeyModifiers *modifiers = message;
        uint8_t leds;
        SpiceKbdInstance *keyboard = inputs_channel_get_keyboard(inputs_channel);

        if (!keyboard) {
            break;
        }
        leds = kbd_get_leds(keyboard);
        if ((modifiers->modifiers & SPICE_KEYBOARD_MODIFIER_FLAGS_SCROLL_LOCK) !=
            (leds & SPICE_KEYBOARD_MODIFIER_FLAGS_SCROLL_LOCK)) {
            kbd_push_scan(keyboard, SCROLL_LOCK_SCAN_CODE);
            kbd_push_scan(keyboard, SCROLL_LOCK_SCAN_CODE | 0x80);
        }
        if ((modifiers->modifiers & SPICE_KEYBOARD_MODIFIER_FLAGS_NUM_LOCK) !=
            (leds & SPICE_KEYBOARD_MODIFIER_FLAGS_NUM_LOCK)) {
            kbd_push_scan(keyboard, NUM_LOCK_SCAN_CODE);
            kbd_push_scan(keyboard, NUM_LOCK_SCAN_CODE | 0x80);
        }
        if ((modifiers->modifiers & SPICE_KEYBOARD_MODIFIER_FLAGS_CAPS_LOCK) !=
            (leds & SPICE_KEYBOARD_MODIFIER_FLAGS_CAPS_LOCK)) {
            kbd_push_scan(keyboard, CAPS_LOCK_SCAN_CODE);
            kbd_push_scan(keyboard, CAPS_LOCK_SCAN_CODE | 0x80);
        }
        activate_modifiers_watch(reds);
        break;
    }
    case SPICE_MSGC_DISCONNECTING:
        break;
    default:
        return red_channel_client_handle_message(rcc, size, type, message);
    }
    return TRUE;
}

static void inputs_release_keys(InputsChannel *inputs)
{
    int i;
    SpiceKbdState *st;
    SpiceKbdInstance *keyboard = inputs_channel_get_keyboard(inputs);

    if (!keyboard) {
        return;
    }
    st = keyboard->st;

    for (i = 0; i < SPICE_N_ELEMENTS(st->key); i++) {
        if (!st->key[i])
            continue;

        st->key[i] = FALSE;
        kbd_push_scan(keyboard, i | 0x80);
    }

    for (i = 0; i < SPICE_N_ELEMENTS(st->key_ext); i++) {
        if (!st->key_ext[i])
            continue;

        st->key_ext[i] = FALSE;
        kbd_push_scan(keyboard, 0xe0);
        kbd_push_scan(keyboard, i | 0x80);
    }
}

static void inputs_channel_on_disconnect(RedChannelClient *rcc)
{
    if (!rcc) {
        return;
    }
    inputs_release_keys(SPICE_CONTAINEROF(rcc->channel, InputsChannel, base));
}

static void inputs_pipe_add_init(RedChannelClient *rcc)
{
    InputsInitPipeItem *item = spice_malloc(sizeof(InputsInitPipeItem));
    InputsChannel *inputs = SPICE_CONTAINEROF(rcc->channel, InputsChannel, base);

    pipe_item_init(&item->base, PIPE_ITEM_INPUTS_INIT);
    item->modifiers = kbd_get_leds(inputs_channel_get_keyboard(inputs));
    red_channel_client_pipe_add_push(rcc, &item->base);
}

static int inputs_channel_config_socket(RedChannelClient *rcc)
{
    int delay_val = 1;
    RedsStream *stream = red_channel_client_get_stream(rcc);

    if (setsockopt(stream->socket, IPPROTO_TCP, TCP_NODELAY,
            &delay_val, sizeof(delay_val)) == -1) {
        if (errno != ENOTSUP && errno != ENOPROTOOPT) {
            spice_printerr("setsockopt failed, %s", strerror(errno));
            return FALSE;
        }
    }

    return TRUE;
}

static void inputs_channel_hold_pipe_item(RedChannelClient *rcc, PipeItem *item)
{
}

static void inputs_connect(RedChannel *channel, RedClient *client,
                           RedsStream *stream, int migration,
                           int num_common_caps, uint32_t *common_caps,
                           int num_caps, uint32_t *caps)
{
    InputsChannelClient *icc;

    if (!reds_stream_is_ssl(stream) && !red_client_during_migrate_at_target(client)) {
        main_channel_client_push_notify(red_client_get_main(client),
                                        "keyboard channel is insecure");
    }

    spice_printerr("inputs channel client create");
    icc = (InputsChannelClient*)red_channel_client_create(sizeof(InputsChannelClient),
                                                          channel,
                                                          client,
                                                          stream,
                                                          FALSE,
                                                          num_common_caps, common_caps,
                                                          num_caps, caps);
    if (!icc) {
        return;
    }
    icc->motion_count = 0;
    inputs_pipe_add_init(&icc->base);
}

static void inputs_migrate(RedChannelClient *rcc)
{
    InputsChannel *inputs = SPICE_CONTAINEROF(rcc->channel, InputsChannel, base);
    inputs->src_during_migrate = TRUE;
    red_channel_client_default_migrate(rcc);
}

static void inputs_channel_push_keyboard_modifiers(InputsChannel *inputs, uint8_t modifiers)
{
    if (!inputs || !red_channel_is_connected(&inputs->base) ||
        inputs->src_during_migrate) {
        return;
    }
    red_channel_pipes_new_add_push(&inputs->base,
        inputs_key_modifiers_item_new, (void*)&modifiers);
}

void inputs_channel_on_keyboard_leds_change(InputsChannel *inputs, uint8_t leds)
{
    inputs_channel_push_keyboard_modifiers(inputs, leds);
}

static void key_modifiers_sender(void *opaque)
{
    InputsChannel *inputs = opaque;
    inputs_channel_push_keyboard_modifiers(inputs, kbd_get_leds(inputs_channel_get_keyboard(inputs)));
}

static int inputs_channel_handle_migrate_flush_mark(RedChannelClient *rcc)
{
    red_channel_client_pipe_add_type(rcc, PIPE_ITEM_MIGRATE_DATA);
    return TRUE;
}

static int inputs_channel_handle_migrate_data(RedChannelClient *rcc,
                                              uint32_t size,
                                              void *message)
{
    InputsChannelClient *icc = SPICE_CONTAINEROF(rcc, InputsChannelClient, base);
    InputsChannel *inputs = SPICE_CONTAINEROF(rcc->channel, InputsChannel, base);
    SpiceMigrateDataHeader *header;
    SpiceMigrateDataInputs *mig_data;

    header = (SpiceMigrateDataHeader *)message;
    mig_data = (SpiceMigrateDataInputs *)(header + 1);

    if (!migration_protocol_validate_header(header,
                                            SPICE_MIGRATE_DATA_INPUTS_MAGIC,
                                            SPICE_MIGRATE_DATA_INPUTS_VERSION)) {
        spice_error("bad header");
        return FALSE;
    }
    key_modifiers_sender(inputs);
    icc->motion_count = mig_data->motion_count;

    for (; icc->motion_count >= SPICE_INPUT_MOTION_ACK_BUNCH;
           icc->motion_count -= SPICE_INPUT_MOTION_ACK_BUNCH) {
        red_channel_client_pipe_add_type(rcc, PIPE_ITEM_MOUSE_MOTION_ACK);
    }
    return TRUE;
}

InputsChannel* inputs_channel_new(RedsState *reds)
{
    ChannelCbs channel_cbs = { NULL, };
    ClientCbs client_cbs = { NULL, };
    InputsChannel *inputs;

    channel_cbs.config_socket = inputs_channel_config_socket;
    channel_cbs.on_disconnect = inputs_channel_on_disconnect;
    channel_cbs.send_item = inputs_channel_send_item;
    channel_cbs.hold_item = inputs_channel_hold_pipe_item;
    channel_cbs.release_item = inputs_channel_release_pipe_item;
    channel_cbs.alloc_recv_buf = inputs_channel_alloc_msg_rcv_buf;
    channel_cbs.release_recv_buf = inputs_channel_release_msg_rcv_buf;
    channel_cbs.handle_migrate_data = inputs_channel_handle_migrate_data;
    channel_cbs.handle_migrate_flush_mark = inputs_channel_handle_migrate_flush_mark;

    inputs = (InputsChannel *)red_channel_create_parser(
                                    sizeof(InputsChannel),
                                    reds,
                                    reds_get_core_interface(reds),
                                    SPICE_CHANNEL_INPUTS, 0,
                                    FALSE, /* handle_acks */
                                    spice_get_client_channel_parser(SPICE_CHANNEL_INPUTS, NULL),
                                    inputs_channel_handle_parsed,
                                    &channel_cbs,
                                    SPICE_MIGRATE_NEED_FLUSH | SPICE_MIGRATE_NEED_DATA_TRANSFER);

    if (!inputs) {
        spice_error("failed to allocate Inputs Channel");
    }

    client_cbs.connect = inputs_connect;
    client_cbs.migrate = inputs_migrate;
    red_channel_register_client_cbs(&inputs->base, &client_cbs, NULL);

    red_channel_set_cap(&inputs->base, SPICE_INPUTS_CAP_KEY_SCANCODE);
    reds_register_channel(reds, &inputs->base);

    if (!(key_modifiers_timer = reds_core_timer_add(reds, key_modifiers_sender, inputs))) {
        spice_error("key modifiers timer create failed");
    }
    return inputs;
}

SpiceKbdInstance* inputs_channel_get_keyboard(InputsChannel *inputs)
{
    return inputs->keyboard;
}

int inputs_channel_set_keyboard(InputsChannel *inputs, SpiceKbdInstance *keyboard)
{
    if (inputs->keyboard) {
        spice_printerr("already have keyboard");
        return -1;
    }
    inputs->keyboard = keyboard;
    inputs->keyboard->st = spice_kbd_state_new(red_channel_get_server(&inputs->base));
    return 0;
}

SpiceMouseInstance* inputs_channel_get_mouse(InputsChannel *inputs)
{
    return inputs->mouse;
}

int inputs_channel_set_mouse(InputsChannel *inputs, SpiceMouseInstance *mouse)
{
    if (inputs->mouse) {
        spice_printerr("already have mouse");
        return -1;
    }
    inputs->mouse = mouse;
    inputs->mouse->st = spice_mouse_state_new();
    return 0;
}

SpiceTabletInstance* inputs_channel_get_tablet(InputsChannel *inputs)
{
    return inputs->tablet;
}

int inputs_channel_set_tablet(InputsChannel *inputs, SpiceTabletInstance *tablet, RedsState *reds)
{
    if (inputs->tablet) {
        spice_printerr("already have tablet");
        return -1;
    }
    inputs->tablet = tablet;
    inputs->tablet->st = spice_tablet_state_new();
    inputs->tablet->st->reds = reds;
    return 0;
}

int inputs_channel_has_tablet(InputsChannel *inputs)
{
    return inputs != NULL && inputs->tablet != NULL;
}

void inputs_channel_detach_tablet(InputsChannel *inputs, SpiceTabletInstance *tablet)
{
    spice_printerr("");
    inputs->tablet = NULL;
}

