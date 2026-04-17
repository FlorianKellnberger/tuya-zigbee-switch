#include "hal/zigbee.h"

#include "app/framework/include/af.h"
#include "app/framework/common/zigbee_app_framework_event.h"
#include "app/framework/plugin/ota-client/ota-client.h"
#include "network-steering.h"
#include <stddef.h>
#include <string.h>
#include "hal/hal.h"
#include "../../zigbee/consts.h"
#include "../../zigbee/relay_cluster.h"
#include "em_wdog.h"

#define MAX_CLUSTERS    32
#define MAX_ATTRS       128

static uint32_t sync_timer_ms = 0;
#define SYNC_DELAY_MS 1000

extern zigbee_relay_cluster *relay_cluster_by_endpoint[10];

sl_zigbee_af_endpoint_type_t      endpoint_type_buffer[ZCL_FIXED_ENDPOINT_COUNT];
sl_zigbee_af_cluster_t            clusters_buffer[MAX_CLUSTERS];
sl_zigbee_af_attribute_metadata_t attributes_buffer[MAX_ATTRS];

hal_zigbee_endpoint *hal_endpoints;
uint8_t hal_endpoints_cnt;
static hal_zcl_activity_callback_t zcl_activity_callback = NULL;

static uint32_t last_steering_attempt_ms = 0;

static void notify_zcl_activity(void) {
    if (zcl_activity_callback != NULL) {
        zcl_activity_callback();
    }
}

hal_zigbee_cluster *find_hal_cluster(uint8_t endpoint,
                                     sl_zigbee_af_cluster_id_t clusterId) {
    return hal_zigbee_find_cluster(hal_endpoints, hal_endpoints_cnt, endpoint,
                                   clusterId);
}

hal_zigbee_attribute *find_hal_attr(uint8_t endpoint,
                                    sl_zigbee_af_cluster_id_t clusterId,
                                    sl_zigbee_af_attribute_id_t attributeId) {
    return hal_zigbee_find_attribute(hal_endpoints, hal_endpoints_cnt, endpoint,
                                     clusterId, attributeId);
}

static uint32_t on_command_callback(sl_service_opcode_t opcode,
                                    sl_service_function_context_t *context) {
    assert(opcode == SL_SERVICE_FUNCTION_TYPE_ZCL_COMMAND);

    sl_zigbee_af_cluster_command_t *cmd =
        (sl_zigbee_af_cluster_command_t *)context->data;
    hal_zigbee_cluster *hal_cluster = find_hal_cluster(
        cmd->apsFrame->destinationEndpoint, cmd->apsFrame->clusterId);
    if (hal_cluster == NULL || hal_cluster->cmd_callback == NULL)
        return SL_ZIGBEE_ZCL_STATUS_UNSUP_COMMAND;

    uint8_t *payload     = NULL;
    uint16_t payload_len = 0;
    if (cmd->bufLen > cmd->payloadStartIndex) {
        payload     = cmd->buffer + cmd->payloadStartIndex;
        payload_len = cmd->bufLen - cmd->payloadStartIndex;
    }

    hal_zigbee_cmd_result_t res = hal_cluster->cmd_callback(
        cmd->apsFrame->destinationEndpoint, cmd->apsFrame->clusterId,
        cmd->commandId, payload, payload_len);
    if (res == HAL_ZIGBEE_CMD_SKIPPED) {
        return SL_ZIGBEE_ZCL_STATUS_UNSUP_COMMAND;
    }
    sl_zigbee_af_status_t status = SL_ZIGBEE_ZCL_STATUS_SUCCESS;
    if (res == HAL_ZIGBEE_INVALID_VALUE) {
        status = SL_ZIGBEE_ZCL_STATUS_INVALID_VALUE;
    } else if (res == HAL_ZIGBEE_MALFORMED_COMMAND) {
        status = SL_ZIGBEE_ZCL_STATUS_MALFORMED_COMMAND;
    } else if (res == HAL_ZIGBEE_ACTION_DENIED) {
        status = SL_ZIGBEE_ZCL_STATUS_ACTION_DENIED;
    }
    sl_zigbee_af_send_immediate_default_response(status);
    return status;
}

bool sl_zigbee_af_pre_command_received_cb(sl_zigbee_af_cluster_command_t *cmd) {
    (void)cmd;
    notify_zcl_activity();
    return false;
}

bool sl_zigbee_af_message_sent_cb(sl_zigbee_outgoing_message_type_t type,
                                  uint16_t indexOrDestination,
                                  sl_zigbee_aps_frame_t *apsFrame,
                                  uint16_t messageTag,
                                  uint8_t *messageContents,
                                  sl_status_t status) {
    // We only log if apsFrame is valid and cluster is On/Off (0x0006)
    if (apsFrame != NULL && apsFrame->clusterId == 0x0006) {
        // Using %02lX because sl_status_t is a long unsigned int
        printf("ZIGBEE: Msg Sent to 0x%04X, Status: 0x%02lX (0x00=Success)\r\n", 
                indexOrDestination, (unsigned long)status);
    }
    
    return false; // Return false so the framework handles any further logic
}

void sl_zigbee_af_main_tick_cb(void) {
    if (sync_timer_ms != 0) {
        uint32_t now = halCommonGetInt32uMillisecondTick();
        
        if (now - sync_timer_ms > SYNC_DELAY_MS) {
            sync_timer_ms = 0; 
            printf("ZIGBEE: Starting Force Sync sequence...\r\n");
            
            for (uint8_t ep = 1; ep < (sizeof(relay_cluster_by_endpoint) / sizeof(relay_cluster_by_endpoint[0])); ep++) {
                zigbee_relay_cluster *cluster = relay_cluster_by_endpoint[ep];
                if (cluster == NULL) {
                    printf("ZIGBEE: Sync Skip EP %d (No Cluster found)\r\n", ep);
                    continue;
                }

                // --- 1. RELAY STATE ---
                (void)sl_zigbee_af_next_sequence();
                uint8_t r_payload[4] = {
                    (uint8_t)(ZCL_ATTR_ONOFF & 0xFF), 
                    (uint8_t)(ZCL_ATTR_ONOFF >> 8), 
                    ZCL_DATA_TYPE_BOOLEAN, 
                    cluster->relay->on
                };
                sl_zigbee_af_fill_command_global_server_to_client_report_attributes(ZCL_CLUSTER_ON_OFF, r_payload, 4);
                sl_zigbee_af_set_command_endpoints(ep, 1);
                sl_zigbee_af_send_command_unicast(SL_ZIGBEE_OUTGOING_DIRECT, 0x0000);
                printf("ZIGBEE: Sync EP %d Relay: %d\r\n", ep, cluster->relay->on);

                // --- 2. INDICATOR MODE (This is what Z2M usually displays) ---
                // We use the variable from the cluster struct directly
                (void)sl_zigbee_af_next_sequence();
                uint8_t m_payload[4] = {
                    (uint8_t)(ZCL_ATTR_ONOFF_INDICATOR_STATE & 0xFF), 
                    (uint8_t)(ZCL_ATTR_ONOFF_INDICATOR_STATE >> 8), 
                    ZCL_DATA_TYPE_ENUM8, 
                    cluster->indicator_led_mode
                };
                sl_zigbee_af_fill_command_global_server_to_client_report_attributes(ZCL_CLUSTER_ON_OFF, m_payload, 4);
                sl_zigbee_af_set_command_endpoints(ep, 1);
                sl_zigbee_af_send_command_unicast(SL_ZIGBEE_OUTGOING_DIRECT, 0x0000);
                printf("ZIGBEE: Sync EP %d Mode: %d\r\n", ep, cluster->indicator_led_mode);

                halCommonDelayMilliseconds(100); 
            }
        }
    }
}

void hal_zigbee_init(hal_zigbee_endpoint *endpoints, uint8_t endpoints_cnt) {
    hal_endpoints     = endpoints;
    hal_endpoints_cnt = endpoints_cnt;

    int total_clusters = 0;
    for (int i = 0; i < endpoints_cnt; i++) {
        total_clusters += endpoints[i].cluster_count;
    }

    if (total_clusters > MAX_CLUSTERS) {
        // You must know if you are over-provisioning memory
        printf("ZIGBEE: FATAL: clusters_buffer overflow (%d > %d)\r\n", total_clusters, MAX_CLUSTERS);
        return; 
    }

    for (int i = 0; i < ZCL_FIXED_ENDPOINT_COUNT; i++) {
        sl_zigbee_af_endpoint_enable_disable(sli_zigbee_af_endpoints[i].endpoint,
                                             false);
    }

    // Avoid settings more endpoints then device supports
    endpoints_cnt = endpoints_cnt <= ZCL_FIXED_ENDPOINT_COUNT
                      ? endpoints_cnt
                      : ZCL_FIXED_ENDPOINT_COUNT;

    sl_zigbee_af_endpoint_type_t *     endpoint_type_ptr = endpoint_type_buffer;
    sl_zigbee_af_cluster_t *           cluster_ptr       = clusters_buffer;
    sl_zigbee_af_attribute_metadata_t *attr_ptr          = attributes_buffer;

    for (int i = 0; i < endpoints_cnt; i++) {
        sli_zigbee_af_endpoints[i].endpoint      = endpoints[i].endpoint;
        sli_zigbee_af_endpoints[i].profileId     = endpoints[i].profile_id;
        sli_zigbee_af_endpoints[i].deviceId      = endpoints[i].device_id;
        sli_zigbee_af_endpoints[i].deviceVersion = endpoints[i].device_version;
        sli_zigbee_af_endpoints[i].endpointType  = endpoint_type_ptr;

        endpoint_type_ptr->clusterCount = endpoints[i].cluster_count;
        endpoint_type_ptr->cluster      = cluster_ptr;
        endpoint_type_ptr->endpointSize = 0;

        hal_zigbee_cluster *clusters = endpoints[i].clusters;

        for (int j = 0; j < endpoints[i].cluster_count; j++) {
            cluster_ptr->clusterId      = clusters[j].cluster_id;
            cluster_ptr->clusterSize    = 0;
            cluster_ptr->attributeCount = clusters[j].attribute_count;
            cluster_ptr->mask           =
                clusters[j].is_server ? CLUSTER_MASK_SERVER : CLUSTER_MASK_CLIENT;
            cluster_ptr->attributes = attr_ptr;

            hal_zigbee_attribute *attributes = clusters[j].attributes;

            for (int l = 0; l < clusters[j].attribute_count; l++) {
                attr_ptr->attributeId   = attributes[l].attribute_id;
                attr_ptr->attributeType = attributes[l].data_type_id;
                attr_ptr->size          = attributes[l].size;
                attr_ptr->mask          = ATTRIBUTE_MASK_EXTERNAL_STORAGE;
                if (attributes[l].flag == ATTR_WRITABLE) {
                    attr_ptr->mask |= ATTRIBUTE_MASK_WRITABLE;
                }
                attr_ptr++;
            }

            if (clusters[j].cmd_callback) {
                sl_zigbee_subscribe_to_zcl_commands(
                    clusters[j].cluster_id, 0xFFFF,
                    clusters[j].is_server ? ZCL_DIRECTION_CLIENT_TO_SERVER
                                  : ZCL_DIRECTION_SERVER_TO_CLIENT,
                    on_command_callback);
            }
            cluster_ptr++;
        }

        sl_zigbee_af_endpoint_enable_disable(sli_zigbee_af_endpoints[i].endpoint,
                                             true);
        endpoint_type_ptr++;
    }
}

void hal_zigbee_notify_attribute_changed(uint8_t endpoint, uint16_t cluster_id,
                                         uint16_t attribute_id) {
    hal_zigbee_cluster *  cluster = find_hal_cluster(endpoint, cluster_id);
    hal_zigbee_attribute *attr    =
        find_hal_attr(endpoint, cluster_id, attribute_id);

    if (attr == NULL) {
        return;
    }
    sl_zigbee_af_reporting_attribute_change_cb(
        endpoint, cluster_id, attribute_id,
        cluster->is_server ? CLUSTER_MASK_SERVER : CLUSTER_MASK_CLIENT, 0,
        attr->data_type_id, attr->value);
}

static hal_attribute_change_callback_t attribute_change_callback = NULL;

void hal_zigbee_register_on_attribute_change_callback(
    hal_attribute_change_callback_t callback) {
    attribute_change_callback = callback;
}

void hal_zigbee_register_on_zcl_activity_callback(
    hal_zcl_activity_callback_t callback) {
    zcl_activity_callback = callback;
}

sl_zigbee_af_status_t sl_zigbee_af_external_attribute_read_cb(
    uint8_t endpoint, sl_zigbee_af_cluster_id_t clusterId,
    sl_zigbee_af_attribute_metadata_t *attributeMetadata,
    uint16_t manufacturerCode, uint8_t *buffer, uint16_t maxReadLength) {
    hal_zigbee_attribute *attr =
        find_hal_attr(endpoint, clusterId, attributeMetadata->attributeId);

    if (attr == NULL) {
        printf("ZIGBEE: ZCL Read Fail: EP %d, Clus 0x%04X, Attr 0x%04X\r\n", 
                endpoint, clusterId, attributeMetadata->attributeId);
        return SL_ZIGBEE_ZCL_STATUS_UNSUPPORTED_ATTRIBUTE;
    }
    if (maxReadLength < attr->size) {
        return SL_ZIGBEE_ZCL_STATUS_INSUFFICIENT_SPACE;
    }

    memmove(buffer, attr->value, attr->size);

    return SL_ZIGBEE_ZCL_STATUS_SUCCESS;
}

sl_zigbee_af_status_t sl_zigbee_af_external_attribute_write_cb(
    uint8_t endpoint, sl_zigbee_af_cluster_id_t clusterId,
    sl_zigbee_af_attribute_metadata_t *attributeMetadata,
    uint16_t manufacturerCode, uint8_t *buffer) {
    hal_zigbee_attribute *attr =
        find_hal_attr(endpoint, clusterId, attributeMetadata->attributeId);

    if (attr == NULL) {
        return SL_ZIGBEE_ZCL_STATUS_UNSUPPORTED_ATTRIBUTE;
    }
    memmove(attr->value, buffer, attr->size);

    if (attribute_change_callback != NULL) {
        attribute_change_callback(endpoint, clusterId, attr->attribute_id);
    }

    return SL_ZIGBEE_ZCL_STATUS_SUCCESS;
}

static bool network_steering_in_progress = false;

hal_zigbee_network_status_t hal_zigbee_get_network_status() {
    sl_zigbee_network_status_t ns = sl_zigbee_af_network_state();

    if (ns == SL_ZIGBEE_JOINED_NETWORK) {
        return HAL_ZIGBEE_NETWORK_JOINED;
    } else if (ns == SL_ZIGBEE_JOINING_NETWORK || network_steering_in_progress) {
        return HAL_ZIGBEE_NETWORK_JOINING;
    } else {
        return HAL_ZIGBEE_NETWORK_NOT_JOINED;
    }
}

static hal_network_status_change_callback_t network_status_change_callback =
    NULL;

static void notify_network_status_change(void) {
    if (network_status_change_callback != NULL) {
        network_status_change_callback(hal_zigbee_get_network_status());
    }
}

void hal_register_on_network_status_change_callback(
    hal_network_status_change_callback_t callback) {
    network_status_change_callback = callback;
}

void sl_zigbee_af_stack_status_cb(sl_status_t status) {
    // Feed the dog during network transitions
    WDOGn_Feed(WDOG0);
    if (status == SL_STATUS_NETWORK_UP) {
        network_steering_in_progress = false;
        printf("ZIGBEE: Network UP. Sync scheduled in %dms...\r\n", SYNC_DELAY_MS);

        // 1. Tell Z2M we are alive
        sl_zigbee_send_device_announcement();
        // start delay to sync the states
        sync_timer_ms = halCommonGetInt32uMillisecondTick();

    } 
    else if (status == SL_STATUS_NETWORK_DOWN) {
        if (hal_zigbee_get_network_status() == HAL_ZIGBEE_NETWORK_NOT_JOINED) {
            printf("ZIGBEE: Network lost. Starting search...\r\n");
            hal_zigbee_start_network_steering();
        }
    }

    notify_network_status_change();
}

void hal_zigbee_leave_network() {
    sl_zigbee_leave_network(SL_ZIGBEE_LEAVE_NWK_WITH_OPTION_REJOIN);
}

void hal_zigbee_start_network_steering() {
    uint32_t now = halCommonGetInt32uMillisecondTick();
    
    // Check if this is the very first attempt since boot
    bool is_first_attempt = (last_steering_attempt_ms == 0);
    bool is_cooldown_over = (now - last_steering_attempt_ms > 30000);

    if (!network_steering_in_progress && (is_first_attempt || is_cooldown_over)) {
        printf("ZIGBEE: NWK Steering Start triggered\r\n");
        last_steering_attempt_ms = now;
        network_steering_in_progress = true;
        sl_zigbee_af_network_steering_start();
    }
}

// Network steering complete callback
void sl_zigbee_af_network_steering_complete_cb(sl_status_t status,
                                               uint8_t totalBeacons,
                                               uint8_t joinAttempts,
                                               uint8_t finalState) {
    (void)status;
    (void)totalBeacons;
    (void)joinAttempts;
    (void)finalState;
    network_steering_in_progress = false;

    notify_network_status_change();
}

static uint8_t make_frame_control(const hal_zigbee_cmd *c) {
    uint8_t fc = 0;

    if (c->cluster_specific)
        fc |= ZCL_CLUSTER_SPECIFIC_COMMAND;
    if (c->direction == HAL_ZIGBEE_DIR_SERVER_TO_CLIENT)
        fc |= ZCL_FRAME_CONTROL_SERVER_TO_CLIENT;
    if (c->disable_default_rsp)
        fc |= ZCL_DISABLE_DEFAULT_RESPONSE_MASK;
    if (c->manufacturer_code)
        fc |= ZCL_MANUFACTURER_SPECIFIC_MASK;
    return fc;
}

static void fill_cmd(const hal_zigbee_cmd *c) {
    sl_zigbee_set_manufacturer_code(c->manufacturer_code);
    uint8_t fc = make_frame_control(c);

    if (c->payload_len == 0 || c->payload == NULL) {
        sl_zigbee_af_fill_external_buffer(fc, c->cluster_id, c->command_id, "");
    } else {
        sl_zigbee_af_fill_external_buffer(fc, c->cluster_id, c->command_id, "b",
                                          c->payload, c->payload_len);
    }
}

hal_zigbee_status_t hal_zigbee_send_cmd_to_bindings(const hal_zigbee_cmd *cmd) {
    if (!cmd)
        return HAL_ZIGBEE_ERR_BAD_ARG;

    if (sl_zigbee_af_network_state() != SL_ZIGBEE_JOINED_NETWORK)
        return HAL_ZIGBEE_ERR_NOT_JOINED;

    fill_cmd(cmd);

    sl_zigbee_af_set_command_endpoints(cmd->endpoint, cmd->endpoint);
    sl_status_t st = sl_zigbee_af_send_command_unicast_to_bindings();
    return (st == SL_STATUS_OK) ? HAL_ZIGBEE_OK : HAL_ZIGBEE_ERR_SEND_FAILED;
}

hal_zigbee_status_t
hal_zigbee_send_report_attr(uint8_t endpoint, uint16_t cluster_id,
                            uint16_t attr_id, uint8_t zcl_type_id,
                            const void *value, uint8_t value_len) {
    if (sl_zigbee_af_network_state() != SL_ZIGBEE_JOINED_NETWORK)
        return HAL_ZIGBEE_ERR_NOT_JOINED;

    // This ensures the NEXT command filled will have a fresh, unique sequence number
    // It works by updating the internal ZCL counter before the buffer is filled.
    (void)sl_zigbee_af_next_sequence(); 

    uint8_t buf[11]; 
    buf[0] = (uint8_t)(attr_id & 0xFF);
    buf[1] = (uint8_t)(attr_id >> 8);
    buf[2] = zcl_type_id;
    if (value_len) memmove(&buf[3], value, value_len);

    sl_status_t st = sl_zigbee_af_fill_command_global_server_to_client_report_attributes(
            cluster_id, buf, 3 + value_len);
    
    if (st != SL_STATUS_OK) return HAL_ZIGBEE_ERR_SEND_FAILED;

    // Use 1 as destination endpoint (standard for coordinators)
    sl_zigbee_af_set_command_endpoints(endpoint, 1);
    
    st = sl_zigbee_af_send_command_unicast_to_bindings();
    return (st == SL_STATUS_OK) ? HAL_ZIGBEE_OK : HAL_ZIGBEE_ERR_SEND_FAILED;
}

hal_zigbee_status_t hal_zigbee_send_announce(void) {
    if (sl_zigbee_send_device_announcement() != SL_STATUS_OK) {
        return HAL_ZIGBEE_ERR_SEND_FAILED;
    }
    return HAL_ZIGBEE_OK;
}

static bool hal_zigbee_poll_rate_supported(void) {
    sl_zigbee_node_type_t node_type = SL_ZIGBEE_UNKNOWN_DEVICE;

    if (sl_zigbee_af_get_node_type(&node_type) != SL_STATUS_OK) {
        return false;
    }

    return node_type == SL_ZIGBEE_END_DEVICE ||
           node_type == SL_ZIGBEE_SLEEPY_END_DEVICE ||
           node_type == SL_ZIGBEE_S2S_INITIATOR_DEVICE ||
           node_type == SL_ZIGBEE_S2S_TARGET_DEVICE;
}

void hal_zigbee_set_poll_rate_ms(uint32_t poll_rate_ms) {
    if (!hal_zigbee_poll_rate_supported()) {
        return;
    }
    // Only set the long poll interval, keep short poll managed by
    // SDK itself for Silabs
    sl_zigbee_af_set_long_poll_interval_ms_cb(poll_rate_ms);
}

uint32_t hal_zigbee_get_poll_rate_ms(void) {
    if (!hal_zigbee_poll_rate_supported()) {
        return 0;
    }

    return sl_zigbee_af_get_long_poll_interval_ms_cb();
}

void hal_zigbee_init_ota() {
}
