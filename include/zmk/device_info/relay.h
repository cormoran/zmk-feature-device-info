/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/device_info/device_info.pb.h>

/*
 * Handle a GetPeripheralDeviceInfoRequest on the split central: broadcast a
 * collect request to all connected peripherals and answer the RPC immediately
 * with an Ok acknowledgement. Each peripheral's info arrives later as a
 * PeripheralDeviceInfo notification (see src/split/device_info_relay.c).
 *
 * Only defined when CONFIG_ZMK_DEVICE_INFO_SPLIT is enabled on the central
 * role; the Studio handler guards the call site on the same symbol.
 */
void device_info_relay_central_query(const zmk_device_info_GetPeripheralDeviceInfoRequest *req,
                                     zmk_device_info_Response *resp);
