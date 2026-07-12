/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

/*
 * Split event-relay carriers for peripheral device-info collection.
 *
 * `device_info_relay_query` travels central -> peripheral (identifier "DIq"),
 * `device_info_relay_reply` travels peripheral -> central (identifier "DIr").
 *
 * There is only one query (collect device info), so the query carries no
 * payload beyond a correlation nonce; the peripheral answers by encoding its
 * own DeviceInfoResponse (via device_info_fill) into the reply. See
 * src/split/device_info_relay.c.
 *
 * The whole struct is copied into the relay payload, so it must fit
 * CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN (asserted by the relay macros and
 * below). The reply data buffer is sized to the encoded DeviceInfoResponse; a
 * peripheral with a very long Zephyr device list drops that list (truncated=1)
 * so the rest of the info still fits.
 */

/*
 * Largest encoded DeviceInfoResponse carried back to the central. The fixed
 * build/hardware/config/runtime fields encode to a few hundred bytes; the rest
 * is headroom for the Zephyr device list. Kept in sync with the
 * PeripheralDeviceInfo.info max_size in proto/.../device_info.options.
 */
#define DEVICE_INFO_RELAY_REPLY_DATA_MAX 1024

struct device_info_relay_query {
    uint8_t source; /* ZMK_RELAY_EVENT_SOURCE_SELF on send; sender index+1 on receive */
    uint8_t req_id; /* echoed back in the reply for client correlation */
};

struct device_info_relay_reply {
    uint8_t source;
    uint8_t req_id;
    uint8_t truncated;                              /* 1 if the device list was dropped to fit */
    uint16_t len;                                   /* bytes of `data` in use */
    uint8_t data[DEVICE_INFO_RELAY_REPLY_DATA_MAX]; /* encoded DeviceInfoResponse */
};

/*
 * The relay transport chunks a reply across the split link automatically, but
 * reassembles it into a single CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN buffer, so
 * the whole reply struct must fit there. ZMK's default (128) is far too small;
 * this module raises the default, but a Kconfig `default` can lose parse-order
 * to ZMK's own, so set it explicitly on BOTH split halves if this assert fires.
 */
BUILD_ASSERT(sizeof(struct device_info_relay_reply) <= CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "device info peripheral replies need a larger "
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN on both split halves "
             "(see Kconfig / README)");

ZMK_EVENT_DECLARE(device_info_relay_query);
ZMK_EVENT_DECLARE(device_info_relay_reply);
