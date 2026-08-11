#include <pb_decode.h>
#include <pb_encode.h>
#include <zmk/studio/custom.h>
#include <zmk/device_info/device_info.pb.h>
#include <zmk/device_info/collect.h>

#include <zephyr/kernel.h>

#if IS_ENABLED(CONFIG_ZMK_DEVICE_INFO_SPLIT)
#include <zmk/device_info/relay.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta device_info_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("http://cormoran.github.io/zmk-feature-device-info/"),
#if IS_ENABLED(CONFIG_ZMK_DEVICE_INFO_STUDIO_RPC_REQUIRE_UNLOCK)
    .security = ZMK_STUDIO_RPC_HANDLER_SECURED,
#else
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
#endif
};

ZMK_RPC_CUSTOM_SUBSYSTEM(zmk__device_info, &device_info_meta, device_info_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(zmk__device_info, zmk_device_info_Response);

static bool device_info_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                           pb_callback_t *encode_response);

static int handle_get_device_info(const zmk_device_info_GetDeviceInfoRequest *req,
                                  zmk_device_info_Response *resp) {
    ARG_UNUSED(req);
    device_info_fill(&resp->response_type.device_info);
    resp->which_response_type = zmk_device_info_Response_device_info_tag;
    return 0;
}

static bool device_info_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                           pb_callback_t *encode_response) {
    zmk_device_info_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(zmk__device_info, encode_response);

    zmk_device_info_Request req = zmk_device_info_Request_init_zero;

    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, zmk_device_info_Request_fields, &req)) {
        LOG_WRN("Failed to decode device info request: %s", PB_GET_ERROR(&req_stream));
        zmk_device_info_ErrorResponse err = zmk_device_info_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to decode request");
        resp->which_response_type = zmk_device_info_Response_error_tag;
        resp->response_type.error = err;
        return true;
    }

    int rc = 0;
    switch (req.which_request_type) {
    case zmk_device_info_Request_get_device_info_tag:
        rc = handle_get_device_info(&req.request_type.get_device_info, resp);
        break;
#if IS_ENABLED(CONFIG_ZMK_DEVICE_INFO_SPLIT)
    case zmk_device_info_Request_get_peripheral_device_info_tag:
        // Central-only relay plumbing: broadcast a collect request to the
        // peripheral half/halves and acknowledge immediately. Each peripheral's
        // info is delivered later as a PeripheralDeviceInfo notification.
        device_info_relay_central_query(&req.request_type.get_peripheral_device_info, resp);
        break;
#endif
    default:
        LOG_WRN("Unsupported device info request type: %d", req.which_request_type);
        rc = -1;
    }

    if (rc != 0) {
        zmk_device_info_ErrorResponse err = zmk_device_info_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to process request");
        resp->which_response_type = zmk_device_info_Response_error_tag;
        resp->response_type.error = err;
    }
    return true;
}
