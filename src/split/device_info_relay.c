/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <pb_encode.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zmk/event_manager.h>
#include <zmk/device_info/device_info.pb.h>
#include <zmk/device_info/collect.h>
#include <zmk/device_info/relay.h>
#include <zmk/device_info/relay_events.h>

#if IS_ENABLED(CONFIG_ZMK_DEVICE_INFO_STUDIO_RPC)
#include <zmk/studio/custom.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Wire both relay carriers in both directions. The direction macros are
 * self-role-gating (the wrong-direction one expands empty per role), and the
 * HANDLE macros only fire when a relay frame with the matching identifier is
 * actually received -- a central never receives "DIq" and a peripheral never
 * receives "DIr", so listing all four here is safe on either role.
 *
 *   central   --DIq (query)-->  peripheral   (CENTRAL_TO_PERIPHERAL + HANDLE)
 *   peripheral --DIr (reply)--> central      (PERIPHERAL_TO_CENTRAL + HANDLE)
 */
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(device_info_relay_query, DIq, source)
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(device_info_relay_reply, DIr, source)
ZMK_RELAY_EVENT_HANDLE(device_info_relay_query, DIq, source)
ZMK_RELAY_EVENT_HANDLE(device_info_relay_reply, DIr, source)

/*
 * Dedicated work queue for the heavy half of the relay.
 *
 * The relay carriers are re-raised locally by ZMK_RELAY_EVENT_HANDLE from the
 * split relay-receive path, which runs on the SYSTEM work queue. Answering a
 * query (peripheral: collect + a series of pb_encodes, blocking on the split TX
 * queue between frames) and -- worse -- raising a Studio notification (central:
 * raise_zmk_studio_custom_notification builds a full zmk_studio_Notification AND
 * zmk_studio_Response and runs a double pb_encode, all synchronously) needs far
 * more stack than the ~2 KB system work queue has spare after the BLE receive
 * path. Doing it there overflows the sysworkq stack. So the event listeners
 * below only copy the carrier into a msgq and kick this queue; the real work
 * runs here with an RPC-thread-sized stack.
 */
static K_THREAD_STACK_DEFINE(device_info_relay_stack, CONFIG_ZMK_DEVICE_INFO_RELAY_STACK_SIZE);
static struct k_work_q device_info_relay_workq;

static int device_info_relay_workq_init(void) {
    struct k_work_queue_config cfg = {.name = "di_relay"};
    k_work_queue_start(&device_info_relay_workq, device_info_relay_stack,
                       K_THREAD_STACK_SIZEOF(device_info_relay_stack),
                       K_LOWEST_APPLICATION_THREAD_PRIO, &cfg);
    return 0;
}

SYS_INIT(device_info_relay_workq_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/*
 * Peripheral side: a relayed query re-raised locally by
 * ZMK_RELAY_EVENT_HANDLE(device_info_relay_query). Answer it -- against THIS
 * peripheral's own state -- on the relay work queue by emitting a SEQUENCE of
 * small replies (one per info group, plus paged device-list frames), each of
 * which the central turns into a PeripheralDeviceInfo notification. Splitting
 * the info this way keeps every relay frame small (within a 256 B DATA_LEN)
 * instead of shipping one large DeviceInfoResponse.
 *
 * The work queue is single-threaded and drains the msgq serially, so the static
 * scratch buffers below need no extra locking and keep the work-queue stack free
 * for the pb_encode.
 */
K_MSGQ_DEFINE(device_info_relay_query_msgq, sizeof(struct device_info_relay_query), 2, 4);

static zmk_device_info_DeviceInfoResponse answer_resp;
static zmk_device_info_ZephyrDevicePage answer_page;
static struct device_info_relay_reply answer_reply;
static struct device_info_relay_query answer_query;

// Encode one group message into answer_reply and relay it back to the central.
static void relay_send_group(uint8_t kind, const pb_msgdesc_t *fields, const void *msg, bool last) {
    answer_reply.source = ZMK_RELAY_EVENT_SOURCE_SELF;
    answer_reply.req_id = answer_query.req_id;
    answer_reply.kind = kind;
    answer_reply.last = last ? 1 : 0;

    pb_ostream_t os = pb_ostream_from_buffer(answer_reply.data, sizeof(answer_reply.data));
    if (!pb_encode(&os, fields, msg)) {
        // Groups are sized to fit the frame; a failure here means this group's
        // fields exceeded the buffer. Skip just this group so the rest still
        // gets through (the client renders whatever groups it received).
        LOG_WRN("Skipping peripheral device info group %u: encode failed (%s)", kind,
                PB_GET_ERROR(&os));
        return;
    }
    answer_reply.len = (uint16_t)os.bytes_written;
    raise_device_info_relay_reply(answer_reply);
}

// Relay the Zephyr device list as one or more paged DEVICES frames. The final
// frame carries `last` to mark the end of the whole collection.
static void relay_send_device_pages(void) {
    const struct device *devices;
    size_t count = z_device_get_all_static(&devices);

    size_t total_named = 0;
    for (size_t i = 0; i < count; i++) {
        if (devices[i].name) {
            total_named++;
        }
    }

    size_t i = 0;
    size_t emitted = 0;
    do {
        answer_page = (zmk_device_info_ZephyrDevicePage)zmk_device_info_ZephyrDevicePage_init_zero;
        while (answer_page.devices_count < DEVICE_INFO_RELAY_DEVICES_PER_PAGE && i < count) {
            if (!devices[i].name) {
                i++;
                continue;
            }
            zmk_device_info_ZephyrDevice *slot = &answer_page.devices[answer_page.devices_count];
            strncpy(slot->name, devices[i].name, sizeof(slot->name) - 1);
            slot->ready = device_is_ready(&devices[i]);
            answer_page.devices_count++;
            emitted++;
            i++;
        }
        answer_page.last_page = emitted >= total_named;
        relay_send_group(DEVICE_INFO_KIND_DEVICES, zmk_device_info_ZephyrDevicePage_fields,
                         &answer_page, answer_page.last_page);
    } while (emitted < total_named);
}

static void device_info_relay_answer_work(struct k_work *work) {
    ARG_UNUSED(work);
    while (k_msgq_get(&device_info_relay_query_msgq, &answer_query, K_NO_WAIT) == 0) {
        device_info_fill(&answer_resp);

        relay_send_group(DEVICE_INFO_KIND_BUILD, zmk_device_info_BuildInfo_fields,
                         &answer_resp.build, false);
        relay_send_group(DEVICE_INFO_KIND_HARDWARE, zmk_device_info_HardwareInfo_fields,
                         &answer_resp.hardware, false);
        relay_send_group(DEVICE_INFO_KIND_ZMK_CONFIG, zmk_device_info_ZmkConfig_fields,
                         &answer_resp.zmk_config, false);
        relay_send_group(DEVICE_INFO_KIND_RUNTIME, zmk_device_info_RuntimeStatus_fields,
                         &answer_resp.runtime, false);
        // Device pages carry the final `last` marker for the whole collection.
        relay_send_device_pages();
    }
}

static K_WORK_DEFINE(device_info_relay_answer_work_item, device_info_relay_answer_work);

static int device_info_relay_on_query(const zmk_event_t *eh) {
    const struct device_info_relay_query *query = as_device_info_relay_query(eh);
    if (query == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (k_msgq_put(&device_info_relay_query_msgq, query, K_NO_WAIT) != 0) {
        LOG_WRN("device info relay query queue full, dropping query");
        return ZMK_EV_EVENT_BUBBLE;
    }
    k_work_submit_to_queue(&device_info_relay_workq, &device_info_relay_answer_work_item);
    return ZMK_EV_EVENT_HANDLED;
}

ZMK_LISTENER(device_info_relay_answer, device_info_relay_on_query);
ZMK_SUBSCRIPTION(device_info_relay_answer, device_info_relay_query);

#else // CONFIG_ZMK_SPLIT_ROLE_CENTRAL

/*
 * Central side: a peripheral's reply re-raised locally by
 * ZMK_RELAY_EVENT_HANDLE(device_info_relay_reply), with `source` stamped to the
 * peripheral index+1. Forward it to the connected Studio client as a
 * PeripheralDeviceInfo notification -- on the relay work queue, because
 * raise_zmk_studio_custom_notification encodes the whole notification
 * synchronously and overflows the system work queue stack. Only compiled when
 * the Studio RPC subsystem is present (that is where notifications go).
 */
#if IS_ENABLED(CONFIG_ZMK_DEVICE_INFO_STUDIO_RPC)

#define DEVICE_INFO_SUBSYSTEM_IDENTIFIER "zmk__device_info"

static int device_info_relay_subsystem_index(void) {
    size_t count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &count);
    for (size_t i = 0; i < count; i++) {
        struct zmk_rpc_custom_subsystem *subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &subsys);
        if (strcmp(subsys->identifier, DEVICE_INFO_SUBSYSTEM_IDENTIFIER) == 0) {
            return (int)i;
        }
    }
    return -ENOENT;
}

static bool device_info_relay_encode_peripheral_event(pb_ostream_t *stream, const pb_field_t *field,
                                                      void *const *arg) {
    const zmk_device_info_PeripheralDeviceInfo *event =
        (const zmk_device_info_PeripheralDeviceInfo *)*arg;
    return zmk_rpc_custom_subsystem_encode_response_payload(
        stream, field, zmk_device_info_PeripheralDeviceInfo_fields, event);
}

K_MSGQ_DEFINE(device_info_relay_reply_msgq, sizeof(struct device_info_relay_reply), 4, 4);

static struct device_info_relay_reply notify_reply;
static zmk_device_info_PeripheralDeviceInfo notify_event;

static void device_info_relay_notify_work(struct k_work *work) {
    ARG_UNUSED(work);
    while (k_msgq_get(&device_info_relay_reply_msgq, &notify_reply, K_NO_WAIT) == 0) {
        int index = device_info_relay_subsystem_index();
        if (index < 0) {
            LOG_WRN("device info subsystem not registered, dropping peripheral reply");
            continue;
        }

        notify_event =
            (zmk_device_info_PeripheralDeviceInfo)zmk_device_info_PeripheralDeviceInfo_init_zero;
        notify_event.source = notify_reply.source;
        notify_event.req_id = notify_reply.req_id;
        notify_event.kind = notify_reply.kind;
        notify_event.last = notify_reply.last != 0;
        notify_event.payload.size = MIN(notify_reply.len, sizeof(notify_event.payload.bytes));
        memcpy(notify_event.payload.bytes, notify_reply.data, notify_event.payload.size);

        // encode runs inline within raise_zmk_studio_custom_notification, so
        // pointing at the (static) notify_event is safe.
        raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
            .subsystem_index = (uint8_t)index,
            .encode_payload =
                {
                    .funcs.encode = device_info_relay_encode_peripheral_event,
                    .arg = (void *)&notify_event,
                },
        });
    }
}

static K_WORK_DEFINE(device_info_relay_notify_work_item, device_info_relay_notify_work);

static int device_info_relay_on_reply(const zmk_event_t *eh) {
    const struct device_info_relay_reply *reply = as_device_info_relay_reply(eh);
    if (reply == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (k_msgq_put(&device_info_relay_reply_msgq, reply, K_NO_WAIT) != 0) {
        LOG_WRN("device info relay reply queue full, dropping reply");
        return ZMK_EV_EVENT_BUBBLE;
    }
    k_work_submit_to_queue(&device_info_relay_workq, &device_info_relay_notify_work_item);
    return ZMK_EV_EVENT_HANDLED;
}

ZMK_LISTENER(device_info_relay_notify, device_info_relay_on_reply);
ZMK_SUBSCRIPTION(device_info_relay_notify, device_info_relay_reply);

#endif // CONFIG_ZMK_DEVICE_INFO_STUDIO_RPC

/*
 * Central side entry point for the GetPeripheralDeviceInfoRequest RPC (called
 * from the Studio handler): broadcast a collect request to all connected
 * peripherals, then acknowledge immediately. Replies arrive asynchronously via
 * device_info_relay_on_reply above.
 */
void device_info_relay_central_query(const zmk_device_info_GetPeripheralDeviceInfoRequest *req,
                                     zmk_device_info_Response *resp) {
    struct device_info_relay_query query = {
        .source = ZMK_RELAY_EVENT_SOURCE_SELF,
        .req_id = (uint8_t)req->req_id,
    };

    raise_device_info_relay_query(query);

    resp->which_response_type = zmk_device_info_Response_ok_tag;
    resp->response_type.ok = (zmk_device_info_Ok)zmk_device_info_Ok_init_zero;
}

#endif // CONFIG_ZMK_SPLIT_ROLE_CENTRAL
