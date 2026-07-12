/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <pb_encode.h>
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
 * query (peripheral: collect + encode) and -- worse -- raising a Studio
 * notification (central: raise_zmk_studio_custom_notification builds a full
 * zmk_studio_Notification AND zmk_studio_Response and runs a double pb_encode,
 * all synchronously) needs far more stack than the ~2 KB system work queue has
 * spare after the BLE receive path. Doing it there overflows the sysworkq stack.
 * So the event listeners below only copy the carrier into a msgq and kick this
 * queue; the real work runs here with an RPC-thread-sized stack.
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
 * peripheral's own build/hardware/config/device state -- on the relay work
 * queue and raise a reply (relayed back to the central).
 *
 * The work queue is single-threaded and drains the msgq serially, so the static
 * scratch buffers below need no extra locking and keep the work-queue stack
 * free for the pb_encode.
 */
K_MSGQ_DEFINE(device_info_relay_query_msgq, sizeof(struct device_info_relay_query), 2, 4);

static zmk_device_info_DeviceInfoResponse answer_resp;
static struct device_info_relay_reply answer_reply;
static struct device_info_relay_query answer_query;

static void device_info_relay_answer_work(struct k_work *work) {
    ARG_UNUSED(work);
    while (k_msgq_get(&device_info_relay_query_msgq, &answer_query, K_NO_WAIT) == 0) {
        answer_reply = (struct device_info_relay_reply){
            .source = ZMK_RELAY_EVENT_SOURCE_SELF,
            .req_id = answer_query.req_id,
        };

        device_info_fill(&answer_resp);

        pb_ostream_t os = pb_ostream_from_buffer(answer_reply.data, sizeof(answer_reply.data));
        if (!pb_encode(&os, zmk_device_info_DeviceInfoResponse_fields, &answer_resp)) {
            // The Zephyr device list can be long; if the full response does not
            // fit the relay reply buffer, drop the list and report the rest so
            // the client still gets build/hardware/config/runtime info.
            LOG_WRN("Peripheral device info too large for relay (%s); dropping device list",
                    PB_GET_ERROR(&os));
            answer_resp.zephyr_devices.funcs.encode = NULL;
            answer_reply.truncated = 1;
            os = pb_ostream_from_buffer(answer_reply.data, sizeof(answer_reply.data));
            if (!pb_encode(&os, zmk_device_info_DeviceInfoResponse_fields, &answer_resp)) {
                LOG_ERR("Failed to encode peripheral device info reply: %s", PB_GET_ERROR(&os));
                continue;
            }
        }
        answer_reply.len = (uint16_t)os.bytes_written;

        raise_device_info_relay_reply(answer_reply);
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

K_MSGQ_DEFINE(device_info_relay_reply_msgq, sizeof(struct device_info_relay_reply), 2, 4);

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

        notify_event = (zmk_device_info_PeripheralDeviceInfo)
            zmk_device_info_PeripheralDeviceInfo_init_zero;
        notify_event.source = notify_reply.source;
        notify_event.req_id = notify_reply.req_id;
        notify_event.device_list_truncated = notify_reply.truncated != 0;
        notify_event.info.size = MIN(notify_reply.len, sizeof(notify_event.info.bytes));
        memcpy(notify_event.info.bytes, notify_reply.data, notify_event.info.size);

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
