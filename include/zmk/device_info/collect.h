/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/device_info/device_info.pb.h>

/*
 * Fill a DeviceInfoResponse with this device's build/hardware/config/runtime
 * info and a callback that lists its Zephyr devices.
 *
 * Shared by the central Studio RPC handler (src/studio/device_info_handler.c)
 * and the split peripheral relay path (src/split/device_info_relay.c), so both
 * report the exact same information gathered against whichever image they run
 * in. Lives outside src/studio/ because the peripheral image has no
 * CONFIG_ZMK_STUDIO.
 *
 * The zephyr_devices field is encoded via a pb_callback (result->zephyr_devices
 * is left as a callback). A caller that must bound the encoded size can clear
 * result->zephyr_devices.funcs.encode = NULL before encoding to drop the list.
 */
void device_info_fill(zmk_device_info_DeviceInfoResponse *result);
