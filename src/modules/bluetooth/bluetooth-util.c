/***
  This file is part of PulseAudio.

  Copyright 2008-2009 Joao Paulo Rechi Vita

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/xmalloc.h>

#include <pulsecore/core-util.h>
#include <pulsecore/shared.h>
#include <pulsecore/dbus-shared.h>

#include "bluetooth-util.h"
#include "a2dp-codecs.h"

#define HFP_AG_ENDPOINT "/MediaEndpoint/HFPAG"
#define HFP_HS_ENDPOINT "/MediaEndpoint/HFPHS"
#define A2DP_SOURCE_ENDPOINT "/MediaEndpoint/A2DPSource"
#define A2DP_SINK_ENDPOINT "/MediaEndpoint/A2DPSink"

#define ENDPOINT_INTROSPECT_XML                                         \
    DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                           \
    "<node>"                                                            \
    " <interface name=\"org.bluez.MediaEndpoint\">"                     \
    "  <method name=\"SetConfiguration\">"                              \
    "   <arg name=\"transport\" direction=\"in\" type=\"o\"/>"          \
    "   <arg name=\"configuration\" direction=\"in\" type=\"ay\"/>"     \
    "  </method>"                                                       \
    "  <method name=\"SelectConfiguration\">"                           \
    "   <arg name=\"capabilities\" direction=\"in\" type=\"ay\"/>"      \
    "   <arg name=\"configuration\" direction=\"out\" type=\"ay\"/>"    \
    "  </method>"                                                       \
    "  <method name=\"ClearConfiguration\">"                            \
    "  </method>"                                                       \
    "  <method name=\"Release\">"                                       \
    "  </method>"                                                       \
    " </interface>"                                                     \
    " <interface name=\"org.freedesktop.DBus.Introspectable\">"         \
    "  <method name=\"Introspect\">"                                    \
    "   <arg name=\"data\" type=\"s\" direction=\"out\"/>"              \
    "  </method>"                                                       \
    " </interface>"                                                     \
    "</node>"

struct pa_bluetooth_discovery {
    PA_REFCNT_DECLARE;

    pa_core *core;
    pa_dbus_connection *connection;
    PA_LLIST_HEAD(pa_dbus_pending, pending);
    pa_hashmap *devices;
    pa_hashmap *transports;
    pa_hook hooks[PA_BLUETOOTH_HOOK_MAX];
    pa_bool_t filter_added;
};

static void get_properties_reply(DBusPendingCall *pending, void *userdata);
static pa_dbus_pending* send_and_add_to_pending(pa_bluetooth_discovery *y, DBusMessage *m, DBusPendingCallNotifyFunction func, void *call_data);
static void found_adapter(pa_bluetooth_discovery *y, const char *path);
static pa_bluetooth_device *found_device(pa_bluetooth_discovery *y, const char* path);

pa_bt_audio_state_t pa_bt_audio_state_from_string(const char* value) {
    pa_assert(value);

    if (pa_streq(value, "disconnected"))
        return PA_BT_AUDIO_STATE_DISCONNECTED;
    else if (pa_streq(value, "connecting"))
        return PA_BT_AUDIO_STATE_CONNECTING;
    else if (pa_streq(value, "connected"))
        return PA_BT_AUDIO_STATE_CONNECTED;
    else if (pa_streq(value, "playing"))
        return PA_BT_AUDIO_STATE_PLAYING;

    return PA_BT_AUDIO_STATE_INVALID;
}

const char *pa_bt_profile_to_string(enum profile profile) {
    switch(profile) {
        case PROFILE_A2DP:
            return "a2dp";
        case PROFILE_A2DP_SOURCE:
            return "a2dp_source";
        case PROFILE_HSP:
            return "hsp";
        case PROFILE_HFGW:
            return "hfgw";
        case PROFILE_OFF:
            pa_assert_not_reached();
    }

    pa_assert_not_reached();
}

static int profile_from_interface(const char *interface, enum profile *p) {
    pa_assert(interface);
    pa_assert(p);

    if (pa_streq(interface, "org.bluez.AudioSink")) {
        *p = PROFILE_A2DP;
        return 0;
    } else if (pa_streq(interface, "org.bluez.AudioSource")) {
        *p = PROFILE_A2DP_SOURCE;
        return 0;
    } else if (pa_streq(interface, "org.bluez.Headset")) {
        *p = PROFILE_HSP;
        return 0;
    } else if (pa_streq(interface, "org.bluez.HandsfreeGateway")) {
        *p = PROFILE_HFGW;
        return 0;
    }

    return -1;
}

static pa_bluetooth_transport_state_t audio_state_to_transport_state(pa_bt_audio_state_t state) {
    switch (state) {
        case PA_BT_AUDIO_STATE_INVALID: /* Typically if state hasn't been received yet */
        case PA_BT_AUDIO_STATE_DISCONNECTED:
        case PA_BT_AUDIO_STATE_CONNECTING:
            return PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED;
        case PA_BT_AUDIO_STATE_CONNECTED:
            return PA_BLUETOOTH_TRANSPORT_STATE_IDLE;
        case PA_BT_AUDIO_STATE_PLAYING:
            return PA_BLUETOOTH_TRANSPORT_STATE_PLAYING;
    }

    pa_assert_not_reached();
}

static pa_bluetooth_uuid *uuid_new(const char *uuid) {
    pa_bluetooth_uuid *u;

    u = pa_xnew(pa_bluetooth_uuid, 1);
    u->uuid = pa_xstrdup(uuid);
    PA_LLIST_INIT(pa_bluetooth_uuid, u);

    return u;
}

static void uuid_free(pa_bluetooth_uuid *u) {
    pa_assert(u);

    pa_xfree(u->uuid);
    pa_xfree(u);
}

static pa_bluetooth_device* device_new(pa_bluetooth_discovery *discovery, const char *path) {
    pa_bluetooth_device *d;
    unsigned i;

    pa_assert(discovery);
    pa_assert(path);

    d = pa_xnew0(pa_bluetooth_device, 1);

    d->discovery = discovery;
    d->dead = FALSE;

    d->device_info_valid = 0;

    d->name = NULL;
    d->path = pa_xstrdup(path);
    d->paired = -1;
    d->alias = NULL;
    PA_LLIST_HEAD_INIT(pa_bluetooth_uuid, d->uuids);
    d->address = NULL;
    d->class = -1;
    d->trusted = -1;

    d->audio_state = PA_BT_AUDIO_STATE_INVALID;

    for (i = 0; i < PA_BLUETOOTH_PROFILE_COUNT; i++)
        d->profile_state[i] = PA_BT_AUDIO_STATE_INVALID;

    return d;
}

static void transport_free(pa_bluetooth_transport *t) {
    pa_assert(t);

    pa_xfree(t->owner);
    pa_xfree(t->path);
    pa_xfree(t->config);
    pa_xfree(t);
}

static void device_free(pa_bluetooth_device *d) {
    pa_bluetooth_uuid *u;
    pa_bluetooth_transport *t;
    unsigned i;

    pa_assert(d);

    for (i = 0; i < PA_BLUETOOTH_PROFILE_COUNT; i++) {
        if (!(t = d->transports[i]))
            continue;

        d->transports[i] = NULL;
        pa_hashmap_remove(d->discovery->transports, t->path);
        t->state = PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED;
        pa_hook_fire(&d->discovery->hooks[PA_BLUETOOTH_HOOK_TRANSPORT_STATE_CHANGED], t);
        transport_free(t);
    }

    while ((u = d->uuids)) {
        PA_LLIST_REMOVE(pa_bluetooth_uuid, d->uuids, u);
        uuid_free(u);
    }

    pa_xfree(d->name);
    pa_xfree(d->path);
    pa_xfree(d->alias);
    pa_xfree(d->address);
    pa_xfree(d);
}

static pa_bool_t device_is_audio_ready(const pa_bluetooth_device *d) {
    unsigned i;

    pa_assert(d);

    if (!d->device_info_valid || d->audio_state == PA_BT_AUDIO_STATE_INVALID)
        return FALSE;

    for (i = 0; i < PA_BLUETOOTH_PROFILE_COUNT; i++)
        if (d->profile_state[i] != PA_BT_AUDIO_STATE_INVALID)
            return TRUE;

    return FALSE;
}

static const char *check_variant_property(DBusMessageIter *i) {
    const char *key;

    pa_assert(i);

    if (dbus_message_iter_get_arg_type(i) != DBUS_TYPE_STRING) {
        pa_log("Property name not a string.");
        return NULL;
    }

    dbus_message_iter_get_basic(i, &key);

    if (!dbus_message_iter_next(i)) {
        pa_log("Property value missing");
        return NULL;
    }

    if (dbus_message_iter_get_arg_type(i) != DBUS_TYPE_VARIANT) {
        pa_log("Property value not a variant.");
        return NULL;
    }

    return key;
}

static int parse_manager_property(pa_bluetooth_discovery *y, DBusMessageIter *i) {
    const char *key;
    DBusMessageIter variant_i;

    pa_assert(y);

    key = check_variant_property(i);
    if (key == NULL)
        return -1;

    dbus_message_iter_recurse(i, &variant_i);

    switch (dbus_message_iter_get_arg_type(&variant_i)) {

        case DBUS_TYPE_ARRAY: {

            DBusMessageIter ai;
            dbus_message_iter_recurse(&variant_i, &ai);

            if (dbus_message_iter_get_arg_type(&ai) == DBUS_TYPE_OBJECT_PATH &&
                pa_streq(key, "Adapters")) {

                while (dbus_message_iter_get_arg_type(&ai) != DBUS_TYPE_INVALID) {
                    const char *value;

                    dbus_message_iter_get_basic(&ai, &value);

                    found_adapter(y, value);

                    dbus_message_iter_next(&ai);
                }
            }

            break;
        }
    }

    return 0;
}

static int parse_adapter_property(pa_bluetooth_discovery *y, DBusMessageIter *i) {
    const char *key;
    DBusMessageIter variant_i;

    pa_assert(y);

    key = check_variant_property(i);
    if (key == NULL)
        return -1;

    dbus_message_iter_recurse(i, &variant_i);

    switch (dbus_message_iter_get_arg_type(&variant_i)) {

        case DBUS_TYPE_ARRAY: {

            DBusMessageIter ai;
            dbus_message_iter_recurse(&variant_i, &ai);

            if (dbus_message_iter_get_arg_type(&ai) == DBUS_TYPE_OBJECT_PATH &&
                pa_streq(key, "Devices")) {

                while (dbus_message_iter_get_arg_type(&ai) != DBUS_TYPE_INVALID) {
                    const char *value;

                    dbus_message_iter_get_basic(&ai, &value);

                    found_device(y, value);

                    dbus_message_iter_next(&ai);
                }
            }

            break;
        }
    }

    return 0;
}

static int parse_device_property(pa_bluetooth_device *d, DBusMessageIter *i) {
    const char *key;
    DBusMessageIter variant_i;

    pa_assert(d);

    key = check_variant_property(i);
    if (key == NULL)
        return -1;

    dbus_message_iter_recurse(i, &variant_i);

/*     pa_log_debug("Parsing property org.bluez.Device.%s", key); */

    switch (dbus_message_iter_get_arg_type(&variant_i)) {

        case DBUS_TYPE_STRING: {

            const char *value;
            dbus_message_iter_get_basic(&variant_i, &value);

            if (pa_streq(key, "Name")) {
                pa_xfree(d->name);
                d->name = pa_xstrdup(value);
            } else if (pa_streq(key, "Alias")) {
                pa_xfree(d->alias);
                d->alias = pa_xstrdup(value);
            } else if (pa_streq(key, "Address")) {
                pa_xfree(d->address);
                d->address = pa_xstrdup(value);
            }

/*             pa_log_debug("Value %s", value); */

            break;
        }

        case DBUS_TYPE_BOOLEAN: {

            dbus_bool_t value;
            dbus_message_iter_get_basic(&variant_i, &value);

            if (pa_streq(key, "Paired"))
                d->paired = !!value;
            else if (pa_streq(key, "Trusted"))
                d->trusted = !!value;

/*             pa_log_debug("Value %s", pa_yes_no(value)); */

            break;
        }

        case DBUS_TYPE_UINT32: {

            uint32_t value;
            dbus_message_iter_get_basic(&variant_i, &value);

            if (pa_streq(key, "Class"))
                d->class = (int) value;

/*             pa_log_debug("Value %u", (unsigned) value); */

            break;
        }

        case DBUS_TYPE_ARRAY: {

            DBusMessageIter ai;
            dbus_message_iter_recurse(&variant_i, &ai);

            if (dbus_message_iter_get_arg_type(&ai) == DBUS_TYPE_STRING &&
                pa_streq(key, "UUIDs")) {
                    DBusMessage *m;
                    pa_bool_t has_audio = FALSE;

                while (dbus_message_iter_get_arg_type(&ai) != DBUS_TYPE_INVALID) {
                    pa_bluetooth_uuid *node;
                    const char *value;
                    struct pa_bluetooth_hook_uuid_data uuiddata;

                    dbus_message_iter_get_basic(&ai, &value);

                    if (pa_bluetooth_uuid_has(d->uuids, value)) {
                        dbus_message_iter_next(&ai);
                        continue;
                    }

                    node = uuid_new(value);
                    PA_LLIST_PREPEND(pa_bluetooth_uuid, d->uuids, node);

                    uuiddata.device = d;
                    uuiddata.uuid = value;
                    pa_hook_fire(&d->discovery->hooks[PA_BLUETOOTH_HOOK_DEVICE_UUID_ADDED], &uuiddata);

                    /* Vudentz said the interfaces are here when the UUIDs are announced */
                    if (strcasecmp(HSP_AG_UUID, value) == 0 || strcasecmp(HFP_AG_UUID, value) == 0) {
                        pa_assert_se(m = dbus_message_new_method_call("org.bluez", d->path, "org.bluez.HandsfreeGateway", "GetProperties"));
                        send_and_add_to_pending(d->discovery, m, get_properties_reply, d);
                        has_audio = TRUE;
                    } else if (strcasecmp(HSP_HS_UUID, value) == 0 || strcasecmp(HFP_HS_UUID, value) == 0) {
                        pa_assert_se(m = dbus_message_new_method_call("org.bluez", d->path, "org.bluez.Headset", "GetProperties"));
                        send_and_add_to_pending(d->discovery, m, get_properties_reply, d);
                        has_audio = TRUE;
                    } else if (strcasecmp(A2DP_SINK_UUID, value) == 0) {
                        pa_assert_se(m = dbus_message_new_method_call("org.bluez", d->path, "org.bluez.AudioSink", "GetProperties"));
                        send_and_add_to_pending(d->discovery, m, get_properties_reply, d);
                        has_audio = TRUE;
                    } else if (strcasecmp(A2DP_SOURCE_UUID, value) == 0) {
                        pa_assert_se(m = dbus_message_new_method_call("org.bluez", d->path, "org.bluez.AudioSource", "GetProperties"));
                        send_and_add_to_pending(d->discovery, m, get_properties_reply, d);
                        has_audio = TRUE;
                    }

                    dbus_message_iter_next(&ai);
                }

                /* this might eventually be racy if .Audio is not there yet, but the State change will come anyway later, so this call is for cold-detection mostly */
                if (has_audio) {
                    pa_assert_se(m = dbus_message_new_method_call("org.bluez", d->path, "org.bluez.Audio", "GetProperties"));
                    send_and_add_to_pending(d->discovery, m, get_properties_reply, d);
                }
            }

            break;
        }
    }

    return 0;
}

static int parse_audio_property(pa_bluetooth_device *d, const char *interface, DBusMessageIter *i) {
    pa_bluetooth_transport *transport;
    const char *key;
    DBusMessageIter variant_i;
    bool is_audio_interface;
    enum profile p = PROFILE_OFF;

    pa_assert(d);
    pa_assert(interface);
    pa_assert(i);

    if (!(is_audio_interface = pa_streq(interface, "org.bluez.Audio")))
        if (profile_from_interface(interface, &p) < 0)
            return 0; /* Interface not known so silently ignore property */

    key = check_variant_property(i);
    if (key == NULL)
        return -1;

    transport = p == PROFILE_OFF ? NULL : d->transports[p];

    dbus_message_iter_recurse(i, &variant_i);

/*     pa_log_debug("Parsing property org.bluez.{Audio|AudioSink|AudioSource|Headset}.%s", key); */

    switch (dbus_message_iter_get_arg_type(&variant_i)) {

        case DBUS_TYPE_STRING: {

            const char *value;
            dbus_message_iter_get_basic(&variant_i, &value);

            if (pa_streq(key, "State")) {
                pa_bt_audio_state_t state = pa_bt_audio_state_from_string(value);
                pa_bluetooth_transport_state_t old_state;

                pa_log_debug("Device %s interface %s property 'State' changed to value '%s'", d->path, interface, value);

                if (state == PA_BT_AUDIO_STATE_INVALID)
                    return -1;

                if (is_audio_interface) {
                    d->audio_state = state;
                    break;
                }

                pa_assert(p != PROFILE_OFF);

                d->profile_state[p] = state;

                if (!transport)
                    break;

                old_state = transport->state;
                transport->state = audio_state_to_transport_state(state);

                if (transport->state != old_state)
                    pa_hook_fire(&d->discovery->hooks[PA_BLUETOOTH_HOOK_TRANSPORT_STATE_CHANGED], transport);
            }

            break;
        }

        case DBUS_TYPE_UINT16: {
            uint16_t value;

            dbus_message_iter_get_basic(&variant_i, &value);

            if (pa_streq(key, "MicrophoneGain")) {
                uint16_t gain;

                pa_log_debug("dbus: property '%s' changed to value '%u'", key, value);

                if (!transport) {
                    pa_log("Volume change does not have an associated transport");
                    return -1;
                }

                if ((gain = PA_MIN(value, HSP_MAX_GAIN)) == transport->microphone_gain)
                    break;

                transport->microphone_gain = gain;
                pa_hook_fire(&d->discovery->hooks[PA_BLUETOOTH_HOOK_TRANSPORT_MICROPHONE_GAIN_CHANGED], transport);
            } else if (pa_streq(key, "SpeakerGain")) {
                uint16_t gain;

                pa_log_debug("dbus: property '%s' changed to value '%u'", key, value);

                if (!transport) {
                    pa_log("Volume change does not have an associated transport");
                    return -1;
                }

                if ((gain = PA_MIN(value, HSP_MAX_GAIN)) == transport->speaker_gain)
                    break;

                transport->speaker_gain = gain;
                pa_hook_fire(&d->discovery->hooks[PA_BLUETOOTH_HOOK_TRANSPORT_SPEAKER_GAIN_CHANGED], transport);
            }

            break;
        }
    }

    return 0;
}

static void run_callback(pa_bluetooth_device *d, pa_bool_t dead) {
    pa_assert(d);

    if (!device_is_audio_ready(d))
        return;

    d->dead = dead;
    pa_hook_fire(&d->discovery->hooks[PA_BLUETOOTH_HOOK_DEVICE_CONNECTION_CHANGED], d);
}

static void remove_all_devices(pa_bluetooth_discovery *y) {
    pa_bluetooth_device *d;

    pa_assert(y);

    while ((d = pa_hashmap_steal_first(y->devices))) {
        run_callback(d, TRUE);
        device_free(d);
    }
}

static pa_bluetooth_device *found_device(pa_bluetooth_discovery *y, const char* path) {
    DBusMessage *m;
    pa_bluetooth_device *d;

    pa_assert(y);
    pa_assert(path);

    d = pa_hashmap_get(y->devices, path);
    if (d)
        return d;

    d = device_new(y, path);

    pa_hashmap_put(y->devices, d->path, d);

    pa_assert_se(m = dbus_message_new_method_call("org.bluez", path, "org.bluez.Device", "GetProperties"));
    send_and_add_to_pending(y, m, get_properties_reply, d);

    /* Before we read the other properties (Audio, AudioSink, AudioSource,
     * Headset) we wait that the UUID is read */
    return d;
}

static void get_properties_reply(DBusPendingCall *pending, void *userdata) {
    DBusMessage *r;
    DBusMessageIter arg_i, element_i;
    pa_dbus_pending *p;
    pa_bluetooth_device *d;
    pa_bluetooth_discovery *y;
    int valid;
    bool old_any_connected;

    pa_assert_se(p = userdata);
    pa_assert_se(y = p->context_data);
    pa_assert_se(r = dbus_pending_call_steal_reply(pending));

/*     pa_log_debug("Got %s.GetProperties response for %s", */
/*                  dbus_message_get_interface(p->message), */
/*                  dbus_message_get_path(p->message)); */

    /* We don't use p->call_data here right-away since the device
     * might already be invalidated at this point */

    if (dbus_message_has_interface(p->message, "org.bluez.Manager") ||
        dbus_message_has_interface(p->message, "org.bluez.Adapter"))
        d = NULL;
    else
        d = pa_hashmap_get(y->devices, dbus_message_get_path(p->message));

    pa_assert(p->call_data == d);

    if (d != NULL)
        old_any_connected = pa_bluetooth_device_any_audio_connected(d);

    valid = dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR ? -1 : 1;

    if (dbus_message_is_method_call(p->message, "org.bluez.Device", "GetProperties"))
        d->device_info_valid = valid;

    if (dbus_message_is_error(r, DBUS_ERROR_SERVICE_UNKNOWN)) {
        pa_log_debug("Bluetooth daemon is apparently not available.");
        remove_all_devices(y);
        goto finish2;
    }

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log("%s.GetProperties() failed: %s: %s", dbus_message_get_interface(p->message), dbus_message_get_error_name(r), pa_dbus_get_error_message(r));
        goto finish;
    }

    if (!dbus_message_iter_init(r, &arg_i)) {
        pa_log("GetProperties reply has no arguments.");
        goto finish;
    }

    if (dbus_message_iter_get_arg_type(&arg_i) != DBUS_TYPE_ARRAY) {
        pa_log("GetProperties argument is not an array.");
        goto finish;
    }

    dbus_message_iter_recurse(&arg_i, &element_i);
    while (dbus_message_iter_get_arg_type(&element_i) != DBUS_TYPE_INVALID) {

        if (dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter dict_i;

            dbus_message_iter_recurse(&element_i, &dict_i);

            if (dbus_message_has_interface(p->message, "org.bluez.Manager")) {
                if (parse_manager_property(y, &dict_i) < 0)
                    goto finish;

            } else if (dbus_message_has_interface(p->message, "org.bluez.Adapter")) {
                if (parse_adapter_property(y, &dict_i) < 0)
                    goto finish;

            } else if (dbus_message_has_interface(p->message, "org.bluez.Device")) {
                if (parse_device_property(d, &dict_i) < 0)
                    goto finish;

            } else if (parse_audio_property(d, dbus_message_get_interface(p->message), &dict_i) < 0)
                goto finish;

        }

        dbus_message_iter_next(&element_i);
    }

finish:
    if (d != NULL && old_any_connected != pa_bluetooth_device_any_audio_connected(d))
        run_callback(d, FALSE);

finish2:
    dbus_message_unref(r);

    PA_LLIST_REMOVE(pa_dbus_pending, y->pending, p);
    pa_dbus_pending_free(p);
}

static pa_dbus_pending* send_and_add_to_pending(pa_bluetooth_discovery *y, DBusMessage *m, DBusPendingCallNotifyFunction func, void *call_data) {
    pa_dbus_pending *p;
    DBusPendingCall *call;

    pa_assert(y);
    pa_assert(m);

    pa_assert_se(dbus_connection_send_with_reply(pa_dbus_connection_get(y->connection), m, &call, -1));

    p = pa_dbus_pending_new(pa_dbus_connection_get(y->connection), m, call, y, call_data);
    PA_LLIST_PREPEND(pa_dbus_pending, y->pending, p);
    dbus_pending_call_set_notify(call, func, p, NULL);

    return p;
}

static void register_endpoint_reply(DBusPendingCall *pending, void *userdata) {
    DBusError e;
    DBusMessage *r;
    pa_dbus_pending *p;
    pa_bluetooth_discovery *y;
    char *endpoint;

    pa_assert(pending);

    dbus_error_init(&e);

    pa_assert_se(p = userdata);
    pa_assert_se(y = p->context_data);
    pa_assert_se(endpoint = p->call_data);
    pa_assert_se(r = dbus_pending_call_steal_reply(pending));

    if (dbus_message_is_error(r, DBUS_ERROR_SERVICE_UNKNOWN)) {
        pa_log_debug("Bluetooth daemon is apparently not available.");
        remove_all_devices(y);
        goto finish;
    }

    if (dbus_message_is_error(r, PA_BLUETOOTH_ERROR_NOT_SUPPORTED)) {
        pa_log_info("Couldn't register endpoint %s, because BlueZ is configured to disable the endpoint type.", endpoint);
        goto finish;
    }

    if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        pa_log("org.bluez.Media.RegisterEndpoint() failed: %s: %s", dbus_message_get_error_name(r), pa_dbus_get_error_message(r));
        goto finish;
    }

finish:
    dbus_message_unref(r);

    PA_LLIST_REMOVE(pa_dbus_pending, y->pending, p);
    pa_dbus_pending_free(p);

    pa_xfree(endpoint);
}

static void register_endpoint(pa_bluetooth_discovery *y, const char *path, const char *endpoint, const char *uuid) {
    DBusMessage *m;
    DBusMessageIter i, d;
    uint8_t codec = 0;

    pa_log_debug("Registering %s on adapter %s.", endpoint, path);

    pa_assert_se(m = dbus_message_new_method_call("org.bluez", path, "org.bluez.Media", "RegisterEndpoint"));

    dbus_message_iter_init_append(m, &i);

    dbus_message_iter_append_basic(&i, DBUS_TYPE_OBJECT_PATH, &endpoint);

    dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
                                    DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
                                    &d);

    pa_dbus_append_basic_variant_dict_entry(&d, "UUID", DBUS_TYPE_STRING, &uuid);

    pa_dbus_append_basic_variant_dict_entry(&d, "Codec", DBUS_TYPE_BYTE, &codec);

    if (pa_streq(uuid, HFP_AG_UUID) || pa_streq(uuid, HFP_HS_UUID)) {
        uint8_t capability = 0;
        pa_dbus_append_basic_array_variant_dict_entry(&d, "Capabilities", DBUS_TYPE_BYTE, &capability, 1);
    } else {
        a2dp_sbc_t capabilities;

        capabilities.channel_mode = SBC_CHANNEL_MODE_MONO | SBC_CHANNEL_MODE_DUAL_CHANNEL |
                                    SBC_CHANNEL_MODE_STEREO | SBC_CHANNEL_MODE_JOINT_STEREO;
        capabilities.frequency = SBC_SAMPLING_FREQ_16000 | SBC_SAMPLING_FREQ_32000 |
                                 SBC_SAMPLING_FREQ_44100 | SBC_SAMPLING_FREQ_48000;
        capabilities.allocation_method = SBC_ALLOCATION_SNR | SBC_ALLOCATION_LOUDNESS;
        capabilities.subbands = SBC_SUBBANDS_4 | SBC_SUBBANDS_8;
        capabilities.block_length = SBC_BLOCK_LENGTH_4 | SBC_BLOCK_LENGTH_8 |
                                    SBC_BLOCK_LENGTH_12 | SBC_BLOCK_LENGTH_16;
        capabilities.min_bitpool = MIN_BITPOOL;
        capabilities.max_bitpool = MAX_BITPOOL;

        pa_dbus_append_basic_array_variant_dict_entry(&d, "Capabilities", DBUS_TYPE_BYTE, &capabilities, sizeof(capabilities));
    }

    dbus_message_iter_close_container(&i, &d);

    send_and_add_to_pending(y, m, register_endpoint_reply, pa_xstrdup(endpoint));
}

static void found_adapter(pa_bluetooth_discovery *y, const char *path) {
    DBusMessage *m;

    pa_assert_se(m = dbus_message_new_method_call("org.bluez", path, "org.bluez.Adapter", "GetProperties"));
    send_and_add_to_pending(y, m, get_properties_reply, NULL);

    register_endpoint(y, path, HFP_AG_ENDPOINT, HFP_AG_UUID);
    register_endpoint(y, path, HFP_HS_ENDPOINT, HFP_HS_UUID);
    register_endpoint(y, path, A2DP_SOURCE_ENDPOINT, A2DP_SOURCE_UUID);
    register_endpoint(y, path, A2DP_SINK_ENDPOINT, A2DP_SINK_UUID);
}

static void list_adapters(pa_bluetooth_discovery *y) {
    DBusMessage *m;
    pa_assert(y);

    pa_assert_se(m = dbus_message_new_method_call("org.bluez", "/", "org.bluez.Manager", "GetProperties"));
    send_and_add_to_pending(y, m, get_properties_reply, NULL);
}

static int transport_parse_property(pa_bluetooth_transport *t, DBusMessageIter *i) {
    const char *key;
    DBusMessageIter variant_i;

    key = check_variant_property(i);
    if (key == NULL)
        return -1;

    dbus_message_iter_recurse(i, &variant_i);

    switch (dbus_message_iter_get_arg_type(&variant_i)) {

        case DBUS_TYPE_BOOLEAN: {

            dbus_bool_t value;
            dbus_message_iter_get_basic(&variant_i, &value);

            if (pa_streq(key, "NREC") && t->nrec != value) {
                t->nrec = value;
                pa_log_debug("Transport %s: Property 'NREC' changed to %s.", t->path, t->nrec ? "True" : "False");
                pa_hook_fire(&t->device->discovery->hooks[PA_BLUETOOTH_HOOK_TRANSPORT_NREC_CHANGED], t);
            }

            break;
         }
    }

    return 0;
}

static DBusHandlerResult filter_cb(DBusConnection *bus, DBusMessage *m, void *userdata) {
    DBusError err;
    pa_bluetooth_discovery *y;

    pa_assert(bus);
    pa_assert(m);

    pa_assert_se(y = userdata);

    dbus_error_init(&err);

    pa_log_debug("dbus: interface=%s, path=%s, member=%s\n",
            dbus_message_get_interface(m),
            dbus_message_get_path(m),
            dbus_message_get_member(m));

    if (dbus_message_is_signal(m, "org.bluez.Adapter", "DeviceRemoved")) {
        const char *path;
        pa_bluetooth_device *d;

        if (!dbus_message_get_args(m, &err, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID)) {
            pa_log("Failed to parse org.bluez.Adapter.DeviceRemoved: %s", err.message);
            goto fail;
        }

        pa_log_debug("Device %s removed", path);

        if ((d = pa_hashmap_remove(y->devices, path))) {
            run_callback(d, TRUE);
            device_free(d);
        }

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    } else if (dbus_message_is_signal(m, "org.bluez.Adapter", "DeviceCreated")) {
        const char *path;

        if (!dbus_message_get_args(m, &err, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID)) {
            pa_log("Failed to parse org.bluez.Adapter.DeviceCreated: %s", err.message);
            goto fail;
        }

        pa_log_debug("Device %s created", path);

        found_device(y, path);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    } else if (dbus_message_is_signal(m, "org.bluez.Manager", "AdapterAdded")) {
        const char *path;

        if (!dbus_message_get_args(m, &err, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID)) {
            pa_log("Failed to parse org.bluez.Manager.AdapterAdded: %s", err.message);
            goto fail;
        }

        pa_log_debug("Adapter %s created", path);

        found_adapter(y, path);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    } else if (dbus_message_is_signal(m, "org.bluez.Audio", "PropertyChanged") ||
               dbus_message_is_signal(m, "org.bluez.Headset", "PropertyChanged") ||
               dbus_message_is_signal(m, "org.bluez.AudioSink", "PropertyChanged") ||
               dbus_message_is_signal(m, "org.bluez.AudioSource", "PropertyChanged") ||
               dbus_message_is_signal(m, "org.bluez.HandsfreeGateway", "PropertyChanged") ||
               dbus_message_is_signal(m, "org.bluez.Device", "PropertyChanged")) {

        pa_bluetooth_device *d;

        if ((d = pa_hashmap_get(y->devices, dbus_message_get_path(m)))) {
            DBusMessageIter arg_i;
            bool old_any_connected = pa_bluetooth_device_any_audio_connected(d);

            if (!dbus_message_iter_init(m, &arg_i)) {
                pa_log("Failed to parse PropertyChanged: %s", err.message);
                goto fail;
            }

            if (dbus_message_has_interface(m, "org.bluez.Device")) {
                if (parse_device_property(d, &arg_i) < 0)
                    goto fail;

            } else if (parse_audio_property(d, dbus_message_get_interface(m), &arg_i) < 0)
                goto fail;

            if (old_any_connected != pa_bluetooth_device_any_audio_connected(d))
                run_callback(d, FALSE);
        }

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    } else if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char *name, *old_owner, *new_owner;

        if (!dbus_message_get_args(m, &err,
                                   DBUS_TYPE_STRING, &name,
                                   DBUS_TYPE_STRING, &old_owner,
                                   DBUS_TYPE_STRING, &new_owner,
                                   DBUS_TYPE_INVALID)) {
            pa_log("Failed to parse org.freedesktop.DBus.NameOwnerChanged: %s", err.message);
            goto fail;
        }

        if (pa_streq(name, "org.bluez")) {
            if (old_owner && *old_owner) {
                pa_log_debug("Bluetooth daemon disappeared.");
                remove_all_devices(y);
            }

            if (new_owner && *new_owner) {
                pa_log_debug("Bluetooth daemon appeared.");
                list_adapters(y);
            }
        }

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    } else if (dbus_message_is_signal(m, "org.bluez.MediaTransport", "PropertyChanged")) {
        pa_bluetooth_transport *t;
        DBusMessageIter arg_i;

        if (!(t = pa_hashmap_get(y->transports, dbus_message_get_path(m))))
            goto fail;

        if (!dbus_message_iter_init(m, &arg_i)) {
            pa_log("Failed to parse PropertyChanged: %s", err.message);
            goto fail;
        }

        if (transport_parse_property(t, &arg_i) < 0)
            goto fail;

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

fail:
    dbus_error_free(&err);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

pa_bluetooth_device* pa_bluetooth_discovery_get_by_address(pa_bluetooth_discovery *y, const char* address) {
    pa_bluetooth_device *d;
    void *state = NULL;

    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);
    pa_assert(address);

    if (!pa_hook_is_firing(&y->hooks[PA_BLUETOOTH_HOOK_DEVICE_CONNECTION_CHANGED]))
        pa_bluetooth_discovery_sync(y);

    while ((d = pa_hashmap_iterate(y->devices, &state, NULL)))
        if (pa_streq(d->address, address))
            return device_is_audio_ready(d) ? d : NULL;

    return NULL;
}

pa_bluetooth_device* pa_bluetooth_discovery_get_by_path(pa_bluetooth_discovery *y, const char* path) {
    pa_bluetooth_device *d;

    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);
    pa_assert(path);

    if (!pa_hook_is_firing(&y->hooks[PA_BLUETOOTH_HOOK_DEVICE_CONNECTION_CHANGED]))
        pa_bluetooth_discovery_sync(y);

    if ((d = pa_hashmap_get(y->devices, path)))
        if (device_is_audio_ready(d))
            return d;

    return NULL;
}

bool pa_bluetooth_device_any_audio_connected(const pa_bluetooth_device *d) {
    pa_assert(d);

    if (d->dead || !device_is_audio_ready(d))
        return false;

    /* Deliberately ignore audio_sink_state and headset_state since they are
     * reflected in audio_state. This is actually very important in order to
     * make module-card-restore work well with headsets: if the headset
     * supports both HSP and A2DP, one of those profiles is connected first and
     * then the other, and lastly the Audio interface becomes connected.
     * Checking only audio_state means that this function will return false at
     * the time when only the first connection has been made. This is good,
     * because otherwise, if the first connection is for HSP and we would
     * already load a new device module instance, and module-card-restore tries
     * to restore the A2DP profile, that would fail because A2DP is not yet
     * connected. Waiting until the Audio interface gets connected means that
     * both headset profiles will be connected when the device module is
     * loaded. */
    return
        d->audio_state >= PA_BT_AUDIO_STATE_CONNECTED ||
        d->profile_state[PROFILE_A2DP_SOURCE] >= PA_BT_AUDIO_STATE_CONNECTED ||
        d->profile_state[PROFILE_HFGW] >= PA_BT_AUDIO_STATE_CONNECTED;
}

int pa_bluetooth_transport_acquire(pa_bluetooth_transport *t, const char *accesstype, size_t *imtu, size_t *omtu) {
    DBusMessage *m, *r;
    DBusError err;
    int ret;
    uint16_t i, o;

    pa_assert(t);
    pa_assert(t->device);
    pa_assert(t->device->discovery);

    dbus_error_init(&err);

    pa_assert_se(m = dbus_message_new_method_call(t->owner, t->path, "org.bluez.MediaTransport", "Acquire"));
    pa_assert_se(dbus_message_append_args(m, DBUS_TYPE_STRING, &accesstype, DBUS_TYPE_INVALID));
    r = dbus_connection_send_with_reply_and_block(pa_dbus_connection_get(t->device->discovery->connection), m, -1, &err);

    if (dbus_error_is_set(&err) || !r) {
        dbus_error_free(&err);
        return -1;
    }

    if (!dbus_message_get_args(r, &err, DBUS_TYPE_UNIX_FD, &ret, DBUS_TYPE_UINT16, &i, DBUS_TYPE_UINT16, &o, DBUS_TYPE_INVALID)) {
        pa_log("Failed to parse org.bluez.MediaTransport.Acquire(): %s", err.message);
        ret = -1;
        dbus_error_free(&err);
        goto fail;
    }

    if (imtu)
        *imtu = i;

    if (omtu)
        *omtu = o;

fail:
    dbus_message_unref(r);
    return ret;
}

void pa_bluetooth_transport_release(pa_bluetooth_transport *t, const char *accesstype) {
    DBusMessage *m;
    DBusError err;

    pa_assert(t);
    pa_assert(t->device);
    pa_assert(t->device->discovery);

    dbus_error_init(&err);

    pa_assert_se(m = dbus_message_new_method_call(t->owner, t->path, "org.bluez.MediaTransport", "Release"));
    pa_assert_se(dbus_message_append_args(m, DBUS_TYPE_STRING, &accesstype, DBUS_TYPE_INVALID));
    dbus_connection_send_with_reply_and_block(pa_dbus_connection_get(t->device->discovery->connection), m, -1, &err);

    if (dbus_error_is_set(&err)) {
        pa_log("Failed to release transport %s: %s", t->path, err.message);
        dbus_error_free(&err);
    } else
        pa_log_info("Transport %s released", t->path);
}

static void set_property(pa_bluetooth_discovery *y, const char *bus, const char *path, const char *interface,
                         const char *prop_name, int prop_type, void *prop_value) {
    DBusMessage *m;
    DBusMessageIter i;

    pa_assert(y);
    pa_assert(path);
    pa_assert(interface);
    pa_assert(prop_name);

    pa_assert_se(m = dbus_message_new_method_call(bus, path, interface, "SetProperty"));
    dbus_message_iter_init_append(m, &i);
    dbus_message_iter_append_basic(&i, DBUS_TYPE_STRING, &prop_name);
    pa_dbus_append_basic_variant(&i, prop_type, prop_value);

    dbus_message_set_no_reply(m, true);
    pa_assert_se(dbus_connection_send(pa_dbus_connection_get(y->connection), m, NULL));
    dbus_message_unref(m);
}

void pa_bluetooth_transport_set_microphone_gain(pa_bluetooth_transport *t, uint16_t value) {
    dbus_uint16_t gain = PA_MIN(value, HSP_MAX_GAIN);

    pa_assert(t);
    pa_assert(t->profile == PROFILE_HSP);

    set_property(t->device->discovery, "org.bluez", t->device->path, "org.bluez.Headset",
                 "MicrophoneGain", DBUS_TYPE_UINT16, &gain);
}

void pa_bluetooth_transport_set_speaker_gain(pa_bluetooth_transport *t, uint16_t value) {
    dbus_uint16_t gain = PA_MIN(value, HSP_MAX_GAIN);

    pa_assert(t);
    pa_assert(t->profile == PROFILE_HSP);

    set_property(t->device->discovery, "org.bluez", t->device->path, "org.bluez.Headset",
                 "SpeakerGain", DBUS_TYPE_UINT16, &gain);
}

static int setup_dbus(pa_bluetooth_discovery *y) {
    DBusError err;

    dbus_error_init(&err);

    y->connection = pa_dbus_bus_get(y->core, DBUS_BUS_SYSTEM, &err);

    if (dbus_error_is_set(&err) || !y->connection) {
        pa_log("Failed to get D-Bus connection: %s", err.message);
        dbus_error_free(&err);
        return -1;
    }

    return 0;
}

static pa_bluetooth_transport *transport_new(pa_bluetooth_device *d, const char *owner, const char *path, enum profile p,
                                             const uint8_t *config, int size) {
    pa_bluetooth_transport *t;

    t = pa_xnew0(pa_bluetooth_transport, 1);
    t->device = d;
    t->owner = pa_xstrdup(owner);
    t->path = pa_xstrdup(path);
    t->profile = p;
    t->config_size = size;

    if (size > 0) {
        t->config = pa_xnew(uint8_t, size);
        memcpy(t->config, config, size);
    }

    t->state = audio_state_to_transport_state(d->profile_state[p]);

    return t;
}

static DBusMessage *endpoint_set_configuration(DBusConnection *conn, DBusMessage *m, void *userdata) {
    pa_bluetooth_discovery *y = userdata;
    pa_bluetooth_device *d;
    pa_bluetooth_transport *t;
    const char *sender, *path, *dev_path = NULL, *uuid = NULL;
    uint8_t *config = NULL;
    int size = 0;
    pa_bool_t nrec = FALSE;
    enum profile p;
    DBusMessageIter args, props;
    DBusMessage *r;

    dbus_message_iter_init(m, &args);

    dbus_message_iter_get_basic(&args, &path);

    if (pa_hashmap_get(y->transports, path)) {
        pa_log("org.bluez.MediaEndpoint.SetConfiguration: Transport %s is already configured.", path);
        goto fail;
    }

    if (!dbus_message_iter_next(&args))
        goto fail;

    dbus_message_iter_recurse(&args, &props);
    if (dbus_message_iter_get_arg_type(&props) != DBUS_TYPE_DICT_ENTRY)
        goto fail;

    /* Read transport properties */
    while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
        const char *key;
        DBusMessageIter value, entry;
        int var;

        dbus_message_iter_recurse(&props, &entry);
        dbus_message_iter_get_basic(&entry, &key);

        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &value);

        var = dbus_message_iter_get_arg_type(&value);
        if (strcasecmp(key, "UUID") == 0) {
            if (var != DBUS_TYPE_STRING)
                goto fail;
            dbus_message_iter_get_basic(&value, &uuid);
        } else if (strcasecmp(key, "Device") == 0) {
            if (var != DBUS_TYPE_OBJECT_PATH)
                goto fail;
            dbus_message_iter_get_basic(&value, &dev_path);
        } else if (strcasecmp(key, "NREC") == 0) {
            dbus_bool_t tmp_boolean;
            if (var != DBUS_TYPE_BOOLEAN)
                goto fail;
            dbus_message_iter_get_basic(&value, &tmp_boolean);
            nrec = tmp_boolean;
        } else if (strcasecmp(key, "Configuration") == 0) {
            DBusMessageIter array;
            if (var != DBUS_TYPE_ARRAY)
                goto fail;
            dbus_message_iter_recurse(&value, &array);
            dbus_message_iter_get_fixed_array(&array, &config, &size);
        }

        dbus_message_iter_next(&props);
    }

    d = found_device(y, dev_path);
    if (!d)
        goto fail;

    if (dbus_message_has_path(m, HFP_AG_ENDPOINT))
        p = PROFILE_HSP;
    else if (dbus_message_has_path(m, HFP_HS_ENDPOINT))
        p = PROFILE_HFGW;
    else if (dbus_message_has_path(m, A2DP_SOURCE_ENDPOINT))
        p = PROFILE_A2DP;
    else
        p = PROFILE_A2DP_SOURCE;

    if (d->transports[p] != NULL) {
        pa_log("Cannot configure transport %s because profile %d is already used", path, p);
        goto fail;
    }

    sender = dbus_message_get_sender(m);

    t = transport_new(d, sender, path, p, config, size);
    if (nrec)
        t->nrec = nrec;

    d->transports[p] = t;
    pa_assert_se(pa_hashmap_put(y->transports, t->path, t) >= 0);

    pa_log_debug("Transport %s profile %d available", t->path, t->profile);

    pa_assert_se(r = dbus_message_new_method_return(m));

    return r;

fail:
    pa_log("org.bluez.MediaEndpoint.SetConfiguration: invalid arguments");
    pa_assert_se(r = (dbus_message_new_error(m, "org.bluez.MediaEndpoint.Error.InvalidArguments",
                                                        "Unable to set configuration")));
    return r;
}

static DBusMessage *endpoint_clear_configuration(DBusConnection *c, DBusMessage *m, void *userdata) {
    pa_bluetooth_discovery *y = userdata;
    pa_bluetooth_transport *t;
    DBusMessage *r;
    DBusError e;
    const char *path;

    dbus_error_init(&e);

    if (!dbus_message_get_args(m, &e, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID)) {
        pa_log("org.bluez.MediaEndpoint.ClearConfiguration: %s", e.message);
        dbus_error_free(&e);
        goto fail;
    }

    if ((t = pa_hashmap_get(y->transports, path))) {
        pa_log_debug("Clearing transport %s profile %d", t->path, t->profile);
        t->device->transports[t->profile] = NULL;
        pa_hashmap_remove(y->transports, t->path);
        t->state = PA_BLUETOOTH_TRANSPORT_STATE_DISCONNECTED;
        pa_hook_fire(&y->hooks[PA_BLUETOOTH_HOOK_TRANSPORT_STATE_CHANGED], t);
        transport_free(t);
    }

    pa_assert_se(r = dbus_message_new_method_return(m));

    return r;

fail:
    pa_assert_se(r = (dbus_message_new_error(m, "org.bluez.MediaEndpoint.Error.InvalidArguments",
                                                        "Unable to clear configuration")));
    return r;
}

static uint8_t a2dp_default_bitpool(uint8_t freq, uint8_t mode) {

    switch (freq) {
        case SBC_SAMPLING_FREQ_16000:
        case SBC_SAMPLING_FREQ_32000:
            return 53;

        case SBC_SAMPLING_FREQ_44100:

            switch (mode) {
                case SBC_CHANNEL_MODE_MONO:
                case SBC_CHANNEL_MODE_DUAL_CHANNEL:
                    return 31;

                case SBC_CHANNEL_MODE_STEREO:
                case SBC_CHANNEL_MODE_JOINT_STEREO:
                    return 53;

                default:
                    pa_log_warn("Invalid channel mode %u", mode);
                    return 53;
            }

        case SBC_SAMPLING_FREQ_48000:

            switch (mode) {
                case SBC_CHANNEL_MODE_MONO:
                case SBC_CHANNEL_MODE_DUAL_CHANNEL:
                    return 29;

                case SBC_CHANNEL_MODE_STEREO:
                case SBC_CHANNEL_MODE_JOINT_STEREO:
                    return 51;

                default:
                    pa_log_warn("Invalid channel mode %u", mode);
                    return 51;
            }

        default:
            pa_log_warn("Invalid sampling freq %u", freq);
            return 53;
    }
}

static DBusMessage *endpoint_select_configuration(DBusConnection *c, DBusMessage *m, void *userdata) {
    pa_bluetooth_discovery *y = userdata;
    a2dp_sbc_t *cap, config;
    uint8_t *pconf = (uint8_t *) &config;
    int i, size;
    DBusMessage *r;
    DBusError e;

    static const struct {
        uint32_t rate;
        uint8_t cap;
    } freq_table[] = {
        { 16000U, SBC_SAMPLING_FREQ_16000 },
        { 32000U, SBC_SAMPLING_FREQ_32000 },
        { 44100U, SBC_SAMPLING_FREQ_44100 },
        { 48000U, SBC_SAMPLING_FREQ_48000 }
    };

    dbus_error_init(&e);

    if (!dbus_message_get_args(m, &e, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &cap, &size, DBUS_TYPE_INVALID)) {
        pa_log("org.bluez.MediaEndpoint.SelectConfiguration: %s", e.message);
        dbus_error_free(&e);
        goto fail;
    }

    if (dbus_message_has_path(m, HFP_AG_ENDPOINT) || dbus_message_has_path(m, HFP_HS_ENDPOINT))
        goto done;

    pa_assert(size == sizeof(config));

    memset(&config, 0, sizeof(config));

    /* Find the lowest freq that is at least as high as the requested
     * sampling rate */
    for (i = 0; (unsigned) i < PA_ELEMENTSOF(freq_table); i++)
        if (freq_table[i].rate >= y->core->default_sample_spec.rate && (cap->frequency & freq_table[i].cap)) {
            config.frequency = freq_table[i].cap;
            break;
        }

    if ((unsigned) i == PA_ELEMENTSOF(freq_table)) {
        for (--i; i >= 0; i--) {
            if (cap->frequency & freq_table[i].cap) {
                config.frequency = freq_table[i].cap;
                break;
            }
        }

        if (i < 0) {
            pa_log("Not suitable sample rate");
            goto fail;
        }
    }

    pa_assert((unsigned) i < PA_ELEMENTSOF(freq_table));

    if (y->core->default_sample_spec.channels <= 1) {
        if (cap->channel_mode & SBC_CHANNEL_MODE_MONO)
            config.channel_mode = SBC_CHANNEL_MODE_MONO;
    }

    if (y->core->default_sample_spec.channels >= 2) {
        if (cap->channel_mode & SBC_CHANNEL_MODE_JOINT_STEREO)
            config.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO;
        else if (cap->channel_mode & SBC_CHANNEL_MODE_STEREO)
            config.channel_mode = SBC_CHANNEL_MODE_STEREO;
        else if (cap->channel_mode & SBC_CHANNEL_MODE_DUAL_CHANNEL)
            config.channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
        else if (cap->channel_mode & SBC_CHANNEL_MODE_MONO) {
            config.channel_mode = SBC_CHANNEL_MODE_MONO;
        } else {
            pa_log("No supported channel modes");
            goto fail;
        }
    }

    if (cap->block_length & SBC_BLOCK_LENGTH_16)
        config.block_length = SBC_BLOCK_LENGTH_16;
    else if (cap->block_length & SBC_BLOCK_LENGTH_12)
        config.block_length = SBC_BLOCK_LENGTH_12;
    else if (cap->block_length & SBC_BLOCK_LENGTH_8)
        config.block_length = SBC_BLOCK_LENGTH_8;
    else if (cap->block_length & SBC_BLOCK_LENGTH_4)
        config.block_length = SBC_BLOCK_LENGTH_4;
    else {
        pa_log_error("No supported block lengths");
        goto fail;
    }

    if (cap->subbands & SBC_SUBBANDS_8)
        config.subbands = SBC_SUBBANDS_8;
    else if (cap->subbands & SBC_SUBBANDS_4)
        config.subbands = SBC_SUBBANDS_4;
    else {
        pa_log_error("No supported subbands");
        goto fail;
    }

    if (cap->allocation_method & SBC_ALLOCATION_LOUDNESS)
        config.allocation_method = SBC_ALLOCATION_LOUDNESS;
    else if (cap->allocation_method & SBC_ALLOCATION_SNR)
        config.allocation_method = SBC_ALLOCATION_SNR;

    config.min_bitpool = (uint8_t) PA_MAX(MIN_BITPOOL, cap->min_bitpool);
    config.max_bitpool = (uint8_t) PA_MIN(a2dp_default_bitpool(config.frequency, config.channel_mode), cap->max_bitpool);

done:
    pa_assert_se(r = dbus_message_new_method_return(m));

    pa_assert_se(dbus_message_append_args(
                                     r,
                                     DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &pconf, size,
                                     DBUS_TYPE_INVALID));

    return r;

fail:
    pa_assert_se(r = (dbus_message_new_error(m, "org.bluez.MediaEndpoint.Error.InvalidArguments",
                                                        "Unable to select configuration")));
    return r;
}

static DBusHandlerResult endpoint_handler(DBusConnection *c, DBusMessage *m, void *userdata) {
    struct pa_bluetooth_discovery *y = userdata;
    DBusMessage *r = NULL;
    DBusError e;
    const char *path;

    pa_assert(y);

    pa_log_debug("dbus: interface=%s, path=%s, member=%s\n",
            dbus_message_get_interface(m),
            dbus_message_get_path(m),
            dbus_message_get_member(m));

    path = dbus_message_get_path(m);
    dbus_error_init(&e);

    if (!pa_streq(path, A2DP_SOURCE_ENDPOINT) && !pa_streq(path, A2DP_SINK_ENDPOINT) && !pa_streq(path, HFP_AG_ENDPOINT) && !pa_streq(path, HFP_HS_ENDPOINT))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        const char *xml = ENDPOINT_INTROSPECT_XML;

        pa_assert_se(r = dbus_message_new_method_return(m));
        pa_assert_se(dbus_message_append_args(
                                 r,
                                 DBUS_TYPE_STRING, &xml,
                                 DBUS_TYPE_INVALID));

    } else if (dbus_message_is_method_call(m, "org.bluez.MediaEndpoint", "SetConfiguration")) {
        r = endpoint_set_configuration(c, m, userdata);
    } else if (dbus_message_is_method_call(m, "org.bluez.MediaEndpoint", "SelectConfiguration")) {
        r = endpoint_select_configuration(c, m, userdata);
    } else if (dbus_message_is_method_call(m, "org.bluez.MediaEndpoint", "ClearConfiguration"))
        r = endpoint_clear_configuration(c, m, userdata);
    else
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (r) {
        pa_assert_se(dbus_connection_send(pa_dbus_connection_get(y->connection), r, NULL));
        dbus_message_unref(r);
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

pa_bluetooth_discovery* pa_bluetooth_discovery_get(pa_core *c) {
    DBusError err;
    pa_bluetooth_discovery *y;
    unsigned i;
    static const DBusObjectPathVTable vtable_endpoint = {
        .message_function = endpoint_handler,
    };

    pa_assert(c);

    dbus_error_init(&err);

    if ((y = pa_shared_get(c, "bluetooth-discovery")))
        return pa_bluetooth_discovery_ref(y);

    y = pa_xnew0(pa_bluetooth_discovery, 1);
    PA_REFCNT_INIT(y);
    y->core = c;
    y->devices = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    y->transports = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    PA_LLIST_HEAD_INIT(pa_dbus_pending, y->pending);

    for (i = 0; i < PA_BLUETOOTH_HOOK_MAX; i++)
        pa_hook_init(&y->hooks[i], y);

    pa_shared_set(c, "bluetooth-discovery", y);

    if (setup_dbus(y) < 0)
        goto fail;

    /* dynamic detection of bluetooth audio devices */
    if (!dbus_connection_add_filter(pa_dbus_connection_get(y->connection), filter_cb, y, NULL)) {
        pa_log_error("Failed to add filter function");
        goto fail;
    }
    y->filter_added = TRUE;

    if (pa_dbus_add_matches(
                pa_dbus_connection_get(y->connection), &err,
                "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'",
                "type='signal',sender='org.bluez',interface='org.bluez.Manager',member='AdapterAdded'",
                "type='signal',sender='org.bluez',interface='org.bluez.Adapter',member='DeviceRemoved'",
                "type='signal',sender='org.bluez',interface='org.bluez.Adapter',member='DeviceCreated'",
                "type='signal',sender='org.bluez',interface='org.bluez.Device',member='PropertyChanged'",
                "type='signal',sender='org.bluez',interface='org.bluez.Audio',member='PropertyChanged'",
                "type='signal',sender='org.bluez',interface='org.bluez.Headset',member='PropertyChanged'",
                "type='signal',sender='org.bluez',interface='org.bluez.AudioSink',member='PropertyChanged'",
                "type='signal',sender='org.bluez',interface='org.bluez.AudioSource',member='PropertyChanged'",
                "type='signal',sender='org.bluez',interface='org.bluez.HandsfreeGateway',member='PropertyChanged'",
                "type='signal',sender='org.bluez',interface='org.bluez.MediaTransport',member='PropertyChanged'",
                NULL) < 0) {
        pa_log("Failed to add D-Bus matches: %s", err.message);
        goto fail;
    }

    pa_assert_se(dbus_connection_register_object_path(pa_dbus_connection_get(y->connection), HFP_AG_ENDPOINT, &vtable_endpoint, y));
    pa_assert_se(dbus_connection_register_object_path(pa_dbus_connection_get(y->connection), HFP_HS_ENDPOINT, &vtable_endpoint, y));
    pa_assert_se(dbus_connection_register_object_path(pa_dbus_connection_get(y->connection), A2DP_SOURCE_ENDPOINT, &vtable_endpoint, y));
    pa_assert_se(dbus_connection_register_object_path(pa_dbus_connection_get(y->connection), A2DP_SINK_ENDPOINT, &vtable_endpoint, y));

    list_adapters(y);

    return y;

fail:

    if (y)
        pa_bluetooth_discovery_unref(y);

    dbus_error_free(&err);

    return NULL;
}

pa_bluetooth_discovery* pa_bluetooth_discovery_ref(pa_bluetooth_discovery *y) {
    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);

    PA_REFCNT_INC(y);

    return y;
}

void pa_bluetooth_discovery_unref(pa_bluetooth_discovery *y) {
    unsigned i;

    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);

    if (PA_REFCNT_DEC(y) > 0)
        return;

    pa_dbus_free_pending_list(&y->pending);

    if (y->devices) {
        remove_all_devices(y);
        pa_hashmap_free(y->devices, NULL, NULL);
    }

    if (y->transports) {
        pa_assert(pa_hashmap_isempty(y->transports));
        pa_hashmap_free(y->transports, NULL, NULL);
    }

    if (y->connection) {
        dbus_connection_unregister_object_path(pa_dbus_connection_get(y->connection), HFP_AG_ENDPOINT);
        dbus_connection_unregister_object_path(pa_dbus_connection_get(y->connection), HFP_HS_ENDPOINT);
        dbus_connection_unregister_object_path(pa_dbus_connection_get(y->connection), A2DP_SOURCE_ENDPOINT);
        dbus_connection_unregister_object_path(pa_dbus_connection_get(y->connection), A2DP_SINK_ENDPOINT);
        pa_dbus_remove_matches(pa_dbus_connection_get(y->connection),
                               "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Manager',member='AdapterAdded'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Manager',member='AdapterRemoved'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Adapter',member='DeviceRemoved'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Adapter',member='DeviceCreated'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Device',member='PropertyChanged'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Audio',member='PropertyChanged'",
                               "type='signal',sender='org.bluez',interface='org.bluez.Headset',member='PropertyChanged'",
                               "type='signal',sender='org.bluez',interface='org.bluez.AudioSink',member='PropertyChanged'",
                               "type='signal',sender='org.bluez',interface='org.bluez.AudioSource',member='PropertyChanged'",
                               "type='signal',sender='org.bluez',interface='org.bluez.HandsfreeGateway',member='PropertyChanged'",
                               "type='signal',sender='org.bluez',interface='org.bluez.MediaTransport',member='PropertyChanged'",
                               NULL);

        if (y->filter_added)
            dbus_connection_remove_filter(pa_dbus_connection_get(y->connection), filter_cb, y);

        pa_dbus_connection_unref(y->connection);
    }

    for (i = 0; i < PA_BLUETOOTH_HOOK_MAX; i++)
        pa_hook_done(&y->hooks[i]);

    if (y->core)
        pa_shared_remove(y->core, "bluetooth-discovery");

    pa_xfree(y);
}

void pa_bluetooth_discovery_sync(pa_bluetooth_discovery *y) {
    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);

    pa_dbus_sync_pending_list(&y->pending);
}

pa_hook* pa_bluetooth_discovery_hook(pa_bluetooth_discovery *y, pa_bluetooth_hook_t hook) {
    pa_assert(y);
    pa_assert(PA_REFCNT_VALUE(y) > 0);

    return &y->hooks[hook];
}

const char*pa_bluetooth_get_form_factor(uint32_t class) {
    unsigned i;
    const char *r;

    static const char * const table[] = {
        [1] = "headset",
        [2] = "hands-free",
        [4] = "microphone",
        [5] = "speaker",
        [6] = "headphone",
        [7] = "portable",
        [8] = "car",
        [10] = "hifi"
    };

    if (((class >> 8) & 31) != 4)
        return NULL;

    if ((i = (class >> 2) & 63) > PA_ELEMENTSOF(table))
        r =  NULL;
    else
        r = table[i];

    if (!r)
        pa_log_debug("Unknown Bluetooth minor device class %u", i);

    return r;
}

char *pa_bluetooth_cleanup_name(const char *name) {
    char *t, *s, *d;
    pa_bool_t space = FALSE;

    pa_assert(name);

    while ((*name >= 1 && *name <= 32) || *name >= 127)
        name++;

    t = pa_xstrdup(name);

    for (s = d = t; *s; s++) {

        if (*s <= 32 || *s >= 127 || *s == '_') {
            space = TRUE;
            continue;
        }

        if (space) {
            *(d++) = ' ';
            space = FALSE;
        }

        *(d++) = *s;
    }

    *d = 0;

    return t;
}

pa_bool_t pa_bluetooth_uuid_has(pa_bluetooth_uuid *uuids, const char *uuid) {
    pa_assert(uuid);

    while (uuids) {
        if (strcasecmp(uuids->uuid, uuid) == 0)
            return TRUE;

        uuids = uuids->next;
    }

    return FALSE;
}
