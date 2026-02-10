/********************************************************************
 * ramses_esp
 * zigbee.c
 *
 * (C) 2026
 *
 * Zigbee bridge using ESP Zigbee stack (ESP32-C6_ZigbeeText pattern)
 *
 */

#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static const char* TAG = "ZIGBEE";
#include "esp_log.h"
#include "esp_check.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

#include "ha/esp_zigbee_ha_standard.h"
#include "esp_zigbee_core.h"

#include "message.h"
#include "zigbee.h"

#define ZIGBEE_TEXT_MAX 256
#define ZIGBEE_QUEUE_LEN 10
#define ZIGBEE_ZHA_MAX_STR_LEN 63
#define ZIGBEE_ZHA_CHUNK_BODY_LEN 54

#define ZB_TEXT_ENDPOINT            10
#define ZB_CLUSTER_TEXT_SEND        0xFC00
#define ZB_CLUSTER_TEXT_RECEIVE     0xFC01
#define ZB_ATTR_TEXT_STRING         0x0000

#define INSTALLCODE_POLICY_ENABLE       false
#define ED_AGING_TIMEOUT                ESP_ZB_ED_AGING_TIMEOUT_64MIN
#define ED_KEEP_ALIVE                   3000

#define ESP_MANUFACTURER_NAME "\x09""ESPRESSIF"
#define ESP_MODEL_IDENTIFIER "\x07"CONFIG_IDF_TARGET

#define ESP_ZB_ZED_CONFIG()                                         \
	{                                                               \
		.esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,                       \
		.install_code_policy = INSTALLCODE_POLICY_ENABLE,           \
		.nwk_cfg.zed_cfg = {                                        \
			.ed_timeout = ED_AGING_TIMEOUT,                         \
			.keep_alive = ED_KEEP_ALIVE,                            \
		},                                                          \
	}


/* Single static buffer for custom commands - stack copies data during cmd_req() call */
static uint8_t sensor_text[ZIGBEE_TEXT_MAX + 1] = {0};
static uint8_t received_text[ZIGBEE_TEXT_MAX + 1] = {0};

#define ZIGBEE_RX_RING_SIZE 4
static char zigbee_rx_ring[ZIGBEE_RX_RING_SIZE][ZIGBEE_TEXT_MAX] = {0};
static size_t zigbee_rx_ring_len[ZIGBEE_RX_RING_SIZE] = {0};
static volatile uint8_t zigbee_rx_ring_head = 0;
static volatile uint8_t zigbee_rx_ring_tail = 0;
static char zigbee_rx_pending_buf[ZIGBEE_TEXT_MAX] = {0};
static volatile size_t zigbee_rx_pending_len = 0;
static volatile bool zigbee_rx_pending_valid = false;

struct zigbee_chunk_state {
	bool active;
	uint8_t total;
	uint8_t next_seq;
	size_t length;
	char buffer[ZIGBEE_TEXT_MAX + 1];
};

static struct zigbee_chunk_state chunk_state = {0};

static bool zigbee_handle_chunked_text(const char* text);
static void zigbee_tx_to_frame(const char* text);
static void zigbee_send_text_chunked(const char* text);
static void zigbee_chunk_reset(void)
{
	chunk_state.active = false;
	chunk_state.total = 0;
	chunk_state.next_seq = 1;
	chunk_state.length = 0;
	chunk_state.buffer[0] = '\0';
}

static bool zigbee_handle_chunked_text(const char* text)
{
	if (!text || !*text)
		return false;

	int header_len = 0;
	unsigned int seq = 0;
	unsigned int total = 0;
	if (sscanf(text, "%u/%u|%n", &seq, &total, &header_len) != 2 || header_len <= 0)
		return false;

	if (total <= 1)
		return false;

	if (seq == 0 || seq > total) {
		ESP_LOGW(TAG, "Zigbee chunk invalid seq=%u total=%u", seq, total);
		zigbee_chunk_reset();
		return true;
	}

	const char* body = text + header_len;
	size_t body_len = strlen(body);
	if (body_len == 0) {
		ESP_LOGW(TAG, "Zigbee chunk %u/%u has empty body", seq, total);
		return true;
	}

	if (!chunk_state.active || seq == 1) {
		zigbee_chunk_reset();
		chunk_state.active = true;
		chunk_state.total = (uint8_t)total;
		chunk_state.next_seq = 1;
	}

	if (chunk_state.next_seq != seq) {
		ESP_LOGW(TAG, "Zigbee chunk out of order (expected %u got %u)", chunk_state.next_seq, seq);
		zigbee_chunk_reset();
		return true;
	}

	if (chunk_state.length + body_len >= sizeof(chunk_state.buffer)) {
		ESP_LOGW(TAG, "Zigbee chunk buffer overflow (len=%u body=%u)", (unsigned int)chunk_state.length, (unsigned int)body_len);
		zigbee_chunk_reset();
		return true;
	}

	memcpy(&chunk_state.buffer[chunk_state.length], body, body_len);
	chunk_state.length += body_len;
	chunk_state.buffer[chunk_state.length] = '\0';
	chunk_state.next_seq++;

	if (seq == total) {
		ESP_LOGI(TAG, "Zigbee chunk assembly complete (%u bytes)", (unsigned int)chunk_state.length);
		zigbee_tx_to_frame(chunk_state.buffer);
		zigbee_chunk_reset();
	}

	return true;
}

enum zigbee_msg_type {
	ZB_MSG_SENSOR_UPDATE,
	ZB_MSG_TX_TO_FRAME,
};

struct zigbee_msg {
	enum zigbee_msg_type type;
	char text[ZIGBEE_TEXT_MAX];
};

struct zigbee_data {
	BaseType_t coreID;
	TaskHandle_t task;
	TaskHandle_t stack_task;
	QueueHandle_t queue;
	zigbee_state_t state;
	bool ready;
};

static struct zigbee_data* zigbee_ctxt(void);

static void zigbee_queue_msg(enum zigbee_msg_type type, const char* text)
{
	struct zigbee_data* ctxt = zigbee_ctxt();
	if (!ctxt || !ctxt->queue || !text)
		return;

	struct zigbee_msg msg = {
		.type = type,
	};
	strncpy(msg.text, text, sizeof(msg.text) - 1);
	msg.text[sizeof(msg.text) - 1] = '\0';

	if (xPortInIsrContext()) {
		BaseType_t higher_priority_woken = pdFALSE;
		xQueueSendFromISR(ctxt->queue, &msg, &higher_priority_woken);
	} else {
		xQueueSend(ctxt->queue, &msg, 0);
	}
}

static void zigbee_store_rx_text_len(const char* text, size_t len)
{
	if (!text || len == 0)
		return;
	if (len > ZIGBEE_TEXT_MAX - 1)
		len = ZIGBEE_TEXT_MAX - 1;

	uint8_t head = zigbee_rx_ring_head;
	uint8_t next = (uint8_t)((head + 1) % ZIGBEE_RX_RING_SIZE);
	if (next == zigbee_rx_ring_tail) {
		return;
	}

	memcpy(zigbee_rx_ring[head], text, len);
	zigbee_rx_ring[head][len] = '\0';
	zigbee_rx_ring_len[head] = len;
	zigbee_rx_ring_head = next;
}


static struct zigbee_data* zigbee_ctxt(void)
{
	static struct zigbee_data zigbee = {
		.state = ZIGBEE_STATE_IDLE,
		.ready = false,
	};
	static struct zigbee_data* ctxt = NULL;
	if (!ctxt) {
		ctxt = &zigbee;
	}

	return ctxt;
}

static char const* zigbee_state_text(zigbee_state_t state)
{
	switch (state) {
	case ZIGBEE_STATE_IDLE:
		return "Idle";
	case ZIGBEE_STATE_PAIRING:
		return "Pairing";
	case ZIGBEE_STATE_CONNECTED:
		return "Connected";
	case ZIGBEE_STATE_ERROR:
		return "Error";
	default:
		return "Unknown";
	}
}

static void zigbee_set_state(struct zigbee_data* ctxt, zigbee_state_t new_state)
{
	if (ctxt && ctxt->state != new_state) {
		ESP_LOGI(TAG, "state %s -> %s", zigbee_state_text(ctxt->state), zigbee_state_text(new_state));
		ctxt->state = new_state;
	}
}

static void zigbee_tx_to_frame(const char* text)
{
	if (!text || text[0] == '\0')
		return;

	struct message* tx = msg_alloc();
	if (!tx) {
		ESP_LOGW(TAG, "TX dropped (no message buffer)");
		return;
	}

	uint8_t err = 0;
	uint8_t done = 0;
	const char* p = text;
	while (!err && *p != '\0')
		err = msg_scan(tx, *(p++));

	if (!err)
		done = msg_scan(tx, '\r');

	if (done && msg_isValid(tx)) {
		msg_tx_ready(&tx);
	} else {
		msg_free(&tx);
		ESP_LOGW(TAG, "TX invalid, dropped <%s>", text);
	}
}

static void handle_rx_text(const uint8_t* data, uint16_t size)
{
	if (!data || size < 1)
		return;

	uint8_t text_len = data[0];
	if (text_len == 0 || size < (uint16_t)(text_len + 1))
		return;

	received_text[0] = text_len;
	memcpy(&received_text[1], data + 1, text_len);
	received_text[text_len + 1] = '\0';

	if (!zigbee_rx_pending_valid) {
		memcpy(zigbee_rx_pending_buf, &received_text[1], text_len);
		zigbee_rx_pending_buf[text_len] = '\0';
		zigbee_rx_pending_len = text_len;
		zigbee_rx_pending_valid = true;
	}
}

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t* message)
{
	if (!message) {
		return ESP_FAIL;
	}
	if (message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
		return ESP_ERR_INVALID_ARG;
	}


	if (message->info.cluster == ZB_CLUSTER_TEXT_RECEIVE &&
		message->attribute.id == ZB_ATTR_TEXT_STRING) {
		if (!message->attribute.data.value || message->attribute.data.size == 0) {
			ESP_LOGW(TAG, "zb_attribute_handler: empty attribute payload");
			return ESP_ERR_INVALID_ARG;
		}
		handle_rx_text((uint8_t*)message->attribute.data.value, message->attribute.data.size);
	}

	return ESP_OK;
}

static esp_err_t zb_custom_cmd_handler(const esp_zb_zcl_custom_cluster_command_message_t* message)
{
	if (!message) {
		return ESP_FAIL;
	}
	if (message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
		return ESP_ERR_INVALID_ARG;
	}

	const uint8_t* data = (const uint8_t*)message->data.value;
	uint16_t size = message->data.size;
	if (!data || size == 0) {
		ESP_LOGW(TAG, "zb_custom_cmd_handler: empty payload");
		return ESP_ERR_INVALID_ARG;
	}

	/* If payload looks like a length-prefixed ZCL char-string, pass through */
	if (size >= 1 && data[0] < size && data[0] < ZIGBEE_TEXT_MAX) {
		handle_rx_text(data, size);
		return ESP_OK;
	}

	/* Otherwise wrap the plain payload into a local length-prefixed buffer */
	uint8_t tmp[ZIGBEE_TEXT_MAX + 1];
	size_t copy_len = size;
	if (copy_len > ZIGBEE_TEXT_MAX - 1)
		copy_len = ZIGBEE_TEXT_MAX - 1;
	tmp[0] = (uint8_t)copy_len;
	memcpy(&tmp[1], data, copy_len);
	handle_rx_text(tmp, (uint16_t)(copy_len + 1));
	return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void* message)
{
	switch (callback_id) {
	case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
		return zb_attribute_handler((const esp_zb_zcl_set_attr_value_message_t*)message);
	case ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_REQ_CB_ID:
		return zb_custom_cmd_handler((const esp_zb_zcl_custom_cluster_command_message_t*)message);
	default:
		return ESP_OK;
	}
}

static void zigbee_send_text_chunked(const char* text)
{
	if (!text || text[0] == '\0')
		return;

	size_t text_len = strlen(text);
	
	if (text_len > ZIGBEE_TEXT_MAX) {
		ESP_LOGW(TAG, "Text too long (%u bytes), truncating to %u", (unsigned)text_len, ZIGBEE_TEXT_MAX);
		text_len = ZIGBEE_TEXT_MAX;
	}

	/* Build length-prefixed ZCL char-string in static buffer */
	sensor_text[0] = (uint8_t)text_len;
	memcpy(&sensor_text[1], text, text_len);
	sensor_text[text_len + 1] = '\0';

	if (!esp_zb_lock_acquire(pdMS_TO_TICKS(1000))) {
		ESP_LOGW(TAG, "Failed to acquire Zigbee lock");
		return;
	}

	/* Send via custom cluster command - stack copies buffer synchronously during this call */
	esp_zb_zcl_custom_cluster_cmd_req_t cmd_req = {
		.zcl_basic_cmd = {
			.dst_addr_u.addr_short = 0x0000,
			.dst_endpoint = 1,
			.src_endpoint = ZB_TEXT_ENDPOINT,
		},
		.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
		.cluster_id = ZB_CLUSTER_TEXT_SEND,
		.profile_id = ESP_ZB_AF_HA_PROFILE_ID,
		.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
		.custom_cmd_id = 0x00,
		.data = {
			.type = ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
			.value = sensor_text,
			.size = (uint16_t)(text_len + 1),
		},
	};

	esp_err_t err = esp_zb_zcl_custom_cluster_cmd_req(&cmd_req);
	esp_zb_lock_release();

	if (err != ESP_OK) {
		ESP_LOGW(TAG, "Custom command send failed: %s", esp_err_to_name(err));
	} else {
		ESP_LOGI(TAG, "Sent text via custom command: %u bytes (APS fragmentation enabled)", (unsigned)text_len);
	}
}


static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
	if (esp_zb_bdb_start_top_level_commissioning(mode_mask) != ESP_OK) {
		ESP_LOGE(TAG, "Failed to start Zigbee commissioning");
	}
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t* signal_struct)
{
	struct zigbee_data* ctxt = zigbee_ctxt();
	uint32_t* p_sg_p = signal_struct->p_app_signal;
	esp_err_t err_status = signal_struct->esp_err_status;
	esp_zb_app_signal_type_t sig_type = *p_sg_p;

	switch (sig_type) {
	case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
		esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
		break;
	case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
	case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
		if (err_status == ESP_OK) {
			zigbee_set_state(ctxt, ZIGBEE_STATE_PAIRING);
			esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
		}
		break;
	case ESP_ZB_BDB_SIGNAL_STEERING:
		if (err_status == ESP_OK) {
			zigbee_set_state(ctxt, ZIGBEE_STATE_CONNECTED);
			ctxt->ready = true;
			
			esp_zb_zcl_start_attr_reporting((esp_zb_zcl_attr_location_info_t){
				.endpoint_id = ZB_TEXT_ENDPOINT,
				.cluster_id = ZB_CLUSTER_TEXT_SEND,
				.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
				.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC,
				.attr_id = ZB_ATTR_TEXT_STRING,
			});
		} else {
			zigbee_set_state(ctxt, ZIGBEE_STATE_ERROR);
			esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
								   ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
		}
		break;
	case ESP_ZB_ZDO_SIGNAL_LEAVE:
		ctxt->ready = false;
		zigbee_set_state(ctxt, ZIGBEE_STATE_ERROR);
		esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
			       ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
		break;
	default:
		break;
	}
}

static void Zigbee(void* param)
{
	struct zigbee_data* ctxt = param;

	ctxt->queue = xQueueCreate(ZIGBEE_QUEUE_LEN, sizeof(struct zigbee_msg));
	if (!ctxt->queue) {
		ESP_LOGE(TAG, "Failed to create queue!");
		vTaskDelete(NULL);
		return;
	}

	while (1) {
		if (zigbee_rx_pending_valid) {
			zigbee_store_rx_text_len(zigbee_rx_pending_buf, zigbee_rx_pending_len);
			zigbee_rx_pending_valid = false;
		}

		while (zigbee_rx_ring_tail != zigbee_rx_ring_head) {
			char* pending_text = zigbee_rx_ring[zigbee_rx_ring_tail];
			if (pending_text[0] != '\0') {
				ESP_LOGI(TAG, "Received Zigbee text: %s", pending_text);
				if (!zigbee_handle_chunked_text(pending_text)) {
					zigbee_tx_to_frame(pending_text);
				}
			}
			zigbee_rx_ring_tail = (uint8_t)((zigbee_rx_ring_tail + 1) % ZIGBEE_RX_RING_SIZE);
		}

		struct zigbee_msg msg;
		BaseType_t res = xQueueReceive(ctxt->queue, &msg, pdMS_TO_TICKS(50));
		if (res) {
			switch (msg.type) {
			case ZB_MSG_SENSOR_UPDATE:
				if (ctxt->ready)
					zigbee_send_text_chunked(msg.text);
				break;
			case ZB_MSG_TX_TO_FRAME:
				ESP_LOGI(TAG, "Received Zigbee text: %s", msg.text);
				if (!zigbee_handle_chunked_text(msg.text)) {
					zigbee_tx_to_frame(msg.text);
				}
				break;
			default:
				break;
			}
		}

		vTaskDelay(pdMS_TO_TICKS(1));
	}
}

static void zigbee_stack_task(void* param)
{
	(void)param;
	esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZED_CONFIG();
	esp_zb_init(&zb_nwk_cfg);

	esp_zb_ep_list_t* ep_list = esp_zb_ep_list_create();
	esp_zb_cluster_list_t* cluster_list = esp_zb_zcl_cluster_list_create();

	esp_zb_attribute_list_t* basic_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_BASIC);
	uint8_t zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE;
	uint8_t power_source = 0x03;
	esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_ZCL_VERSION_ID, &zcl_version);
	esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_POWER_SOURCE_ID, &power_source);
	esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void*)ESP_MANUFACTURER_NAME);
	esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void*)ESP_MODEL_IDENTIFIER);
	esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

	/* Cluster 0xFC00 as CLIENT (output) - sends custom commands to coordinator */
	esp_zb_attribute_list_t* text_send_cluster = esp_zb_zcl_attr_list_create(ZB_CLUSTER_TEXT_SEND);
	esp_zb_cluster_list_add_custom_cluster(cluster_list, text_send_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

	esp_zb_attribute_list_t* text_recv_cluster = esp_zb_zcl_attr_list_create(ZB_CLUSTER_TEXT_RECEIVE);
	esp_zb_custom_cluster_add_custom_attr(text_recv_cluster, ZB_ATTR_TEXT_STRING,
										  ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
										  received_text);
	esp_zb_cluster_list_add_custom_cluster(cluster_list, text_recv_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

	esp_zb_attribute_list_t* identify_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_IDENTIFY);
	uint16_t identify_time = 0;
	esp_zb_identify_cluster_add_attr(identify_cluster, ESP_ZB_ZCL_ATTR_IDENTIFY_IDENTIFY_TIME_ID, &identify_time);
	esp_zb_cluster_list_add_identify_cluster(cluster_list, identify_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

	esp_zb_endpoint_config_t endpoint_config = {
		.endpoint = ZB_TEXT_ENDPOINT,
		.app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
		.app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
		.app_device_version = 0,
	};
	esp_zb_ep_list_add_ep(ep_list, cluster_list, endpoint_config);

	esp_zb_device_register(ep_list);

	esp_zb_core_action_handler_register(zb_action_handler);
	esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);
	ESP_ERROR_CHECK(esp_zb_start(false));

	/* Enable APS fragmentation for large custom command payloads */
	(void)esp_zb_aps_set_fragment_max_window_size(8);
	(void)esp_zb_aps_set_fragment_interframe_delay(10);

	esp_zb_stack_main_loop();
}

void ramses_zigbee_init(BaseType_t coreID)
{
	struct zigbee_data* ctxt = zigbee_ctxt();
	ctxt->coreID = coreID;

	esp_log_level_set(TAG, ESP_LOG_INFO);

	xTaskCreatePinnedToCore(zigbee_stack_task, "ZigbeeStack", 16384, NULL, 5, &ctxt->stack_task, ctxt->coreID);
	xTaskCreatePinnedToCore(Zigbee, "Zigbee", 16384, ctxt, 10, &ctxt->task, ctxt->coreID);
}

void zigbee_update_sensor_text(const char* text)
{
	zigbee_queue_msg(ZB_MSG_SENSOR_UPDATE, text);
}

void zigbee_receive_text(const char* text)
{
	zigbee_queue_msg(ZB_MSG_TX_TO_FRAME, text);
}

zigbee_state_t zigbee_get_state(void)
{
	struct zigbee_data* ctxt = zigbee_ctxt();
	return ctxt ? ctxt->state : ZIGBEE_STATE_ERROR;
}
