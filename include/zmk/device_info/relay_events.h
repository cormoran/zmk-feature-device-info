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
 * payload beyond a correlation nonce. The peripheral answers with a SEQUENCE of
 * replies -- one per info group (build, hardware, config, runtime) plus one per
 * device-list page -- each carrying a small encoded message. This keeps every
 * relayed frame well within CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN (deliberately
 * kept small, <= 256) instead of shipping one large DeviceInfoResponse. See
 * src/split/device_info_relay.c.
 *
 * The whole reply struct is copied into the relay payload, so it must fit
 * CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN (asserted by the relay macros and
 * below).
 */

/*
 * Largest encoded group message carried in one reply. The biggest groups are
 * BuildInfo (a few hundred bytes of git-describe strings in the worst case, ~200
 * in practice) and a device page (<= 5 devices, ~190 B worst case). 248 keeps
 * the whole reply struct within a 256 B relay frame. Kept in sync with the
 * PeripheralDeviceInfo.payload max_size in proto/.../device_info.options.
 */
#define DEVICE_INFO_RELAY_REPLY_DATA_MAX 248

/* Devices per DEVICES page (matches the ZephyrDevicePage max_count in the proto
 * options; 5 * ~38 B < the payload buffer). */
#define DEVICE_INFO_RELAY_DEVICES_PER_PAGE 5

/*
 * Which group a reply / PeripheralDeviceInfo notification carries (the `kind`
 * field). A plain enum rather than a proto enum so the generated web TS stays
 * free of runtime enum syntax; the numeric values are the wire contract shared
 * with the web client (see proto/.../device_info.proto).
 */
enum device_info_peripheral_info_kind {
    DEVICE_INFO_KIND_UNSPECIFIED = 0,
    DEVICE_INFO_KIND_BUILD = 1,      /* payload = BuildInfo */
    DEVICE_INFO_KIND_HARDWARE = 2,   /* payload = HardwareInfo */
    DEVICE_INFO_KIND_ZMK_CONFIG = 3, /* payload = ZmkConfig */
    DEVICE_INFO_KIND_RUNTIME = 4,    /* payload = RuntimeStatus */
    DEVICE_INFO_KIND_DEVICES = 5,    /* payload = ZephyrDevicePage */
};

struct device_info_relay_query {
    uint8_t source; /* ZMK_RELAY_EVENT_SOURCE_SELF on send; sender index+1 on receive */
    uint8_t req_id; /* echoed back in the reply for client correlation */
};

struct device_info_relay_reply {
    uint8_t source;
    uint8_t req_id;
    uint8_t kind;                                   /* enum device_info_peripheral_info_kind */
    uint8_t last;                                   /* 1 on the final reply for this collection */
    uint16_t len;                                   /* bytes of `data` in use */
    uint8_t data[DEVICE_INFO_RELAY_REPLY_DATA_MAX]; /* encoded group message */
};

/*
 * The relay transport chunks a reply across the split link automatically, but
 * reassembles it into a single CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN buffer, so
 * the whole reply struct must fit there. ZMK's default (128) is too small; this
 * module raises the default to 256, but a Kconfig `default` can lose parse-order
 * to ZMK's own, so set it explicitly on BOTH split halves if this assert fires.
 */
BUILD_ASSERT(sizeof(struct device_info_relay_reply) <= CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "device info peripheral replies need "
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN=256 on both split halves "
             "(see Kconfig / README)");

ZMK_EVENT_DECLARE(device_info_relay_query);
ZMK_EVENT_DECLARE(device_info_relay_reply);
