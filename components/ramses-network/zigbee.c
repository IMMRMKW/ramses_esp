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
#include "esp_attr.h"   /* RTC_NOINIT_ATTR */

#include "ha/esp_zigbee_ha_standard.h"
#include "esp_zigbee_core.h"
#include <ramses_buttons.h>
#include "ramses_led.h"

#include "message.h"
#include "zigbee.h"

#define ZIGBEE_TEXT_MAX 256
#define ZIGBEE_QUEUE_LEN 100  /* Increased to handle RF message bursts without drops */
#define ZIGBEE_RX_QUEUE_LEN 8
#define ZIGBEE_ZHA_MAX_STR_LEN 63
#define ZIGBEE_ZHA_CHUNK_BODY_LEN 32  /* Reduced from 54 to prevent APS fragmentation */
#define ZIGBEE_TX_CONFIRM_TIMEOUT_MS 5000  /* Watchdog: clear tx_busy if no confirm in 1s */

#define ZB_TEXT_ENDPOINT            10
#define ZB_CLUSTER_RAMSES_RX        0xFC00  /* RAMSES RX: ESP -> ZHA (client command) */
#define ZB_CLUSTER_RAMSES_TX        0xFC01  /* RAMSES TX: ZHA -> ESP (client command) */


/* RTC memory survives esp_restart() but is cleared on power-on/brown-out.
 * We set this magic value before calling esp_zb_factory_reset() when the
 * coordinator removed us (NOT a user-initiated factory reset).  On the
 * subsequent DEVICE_FIRST_START we check it and skip automatic steering —
 * the device waits for the user to explicitly trigger a join (button / CLI). */
#define ZB_SKIP_AUTOJOIN_MAGIC  0xDEAD5A5A
RTC_NOINIT_ATTR static uint32_t zb_skip_autojoin;

#define INSTALLCODE_POLICY_ENABLE       false
#define ED_AGING_TIMEOUT                ESP_ZB_ED_AGING_TIMEOUT_64MIN
#define ED_KEEP_ALIVE                   3000

#define ESP_MANUFACTURER_NAME "\x07""ELECRAM"
#define ESP_MODEL_IDENTIFIER "\x0ERamses_esp32c6"

#define ESP_ZB_ZED_CONFIG()                                         \
	{                                                               \
		.esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,                       \
		.install_code_policy = INSTALLCODE_POLICY_ENABLE,           \
		.nwk_cfg.zed_cfg = {                                        \
			.ed_timeout = ED_AGING_TIMEOUT,                         \
			.keep_alive = ED_KEEP_ALIVE,                            \
		},                                                          \
	}


/* Single static buffer for received custom command payloads */
static uint8_t received_text[ZIGBEE_TEXT_MAX + 1] = {0};

struct zigbee_rx_msg {
	char text[ZIGBEE_TEXT_MAX];
	size_t len;
};

static QueueHandle_t zigbee_rx_queue = NULL;

struct zigbee_chunk_state {
	bool active;
	uint8_t total;
	uint8_t next_seq;
	size_t length;
	char buffer[ZIGBEE_TEXT_MAX + 1];
};

static struct zigbee_chunk_state chunk_state = {0};

enum zigbee_msg_type {
	ZB_MSG_SENSOR_UPDATE,
	ZB_MSG_TX_TO_FRAME,
	ZB_MSG_SEND_UNACKED,
};

static void zigbee_queue_msg(enum zigbee_msg_type type, const char* text);

static bool zigbee_handle_chunked_text(const char* text);
static void zigbee_tx_to_frame(const char* text);
static void zigbee_send_text_chunked(const char* text);
static void zigbee_send_text_unacked(const char* text);
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
		ESP_LOGD(TAG, "Zigbee chunk assembly complete (%u bytes)", (unsigned int)chunk_state.length);
		zigbee_tx_to_frame(chunk_state.buffer);
		zigbee_chunk_reset();
	}

	return true;
}

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
	volatile bool tx_busy;  /* Single in-flight TX gate to prevent buffer pool exhaustion */
	TickType_t tx_busy_timestamp;  /* Watchdog timestamp for tx_busy */

	/* Application-level ACK tracking */
	volatile uint8_t last_ack_seq;
	volatile uint8_t pending_ack_seq;

	/* Set true when a factory-reset has been requested (via button or console
	 * command).  The SIGNAL_LEAVE handler checks this flag to distinguish a
	 * factory-reset Leave (→ call esp_zb_factory_reset() to erase zb_storage
	 * and restart) from a coordinator-removal Leave. */
	bool factory_reset_pending;
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

	ESP_LOGD(TAG, "Queuing msg type=%d ready=%d tx_busy=%d text=%s", type, ctxt->ready, ctxt->tx_busy, text);

	if (xPortInIsrContext()) {
		BaseType_t higher_priority_woken = pdFALSE;
		xQueueSendFromISR(ctxt->queue, &msg, &higher_priority_woken);
	} else {
		xQueueSend(ctxt->queue, &msg, 0);
	}
}

static void zigbee_enqueue_rx_text(const char* text, size_t len)
{
	if (!text || len == 0 || !zigbee_rx_queue)
		return;
	if (len > ZIGBEE_TEXT_MAX - 1)
		len = ZIGBEE_TEXT_MAX - 1;

	struct zigbee_rx_msg rx_msg;
	memcpy(rx_msg.text, text, len);
	rx_msg.text[len] = '\0';
	rx_msg.len = len;

	if (xPortInIsrContext()) {
		BaseType_t higher_priority_woken = pdFALSE;
		xQueueSendFromISR(zigbee_rx_queue, &rx_msg, &higher_priority_woken);
	} else {
		xQueueSend(zigbee_rx_queue, &rx_msg, 0);
	}
}


static struct zigbee_data* zigbee_ctxt(void)
{
	static struct zigbee_data zigbee = {
		.state = ZIGBEE_STATE_IDLE,
		.ready = false,
		.tx_busy = false,
		.tx_busy_timestamp = 0,
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

	/* Fast-path: if this is an application ACK ("ACK <seq>/<total>")
	 * consume it immediately and update pending state. ACKs must not be
	 * enqueued as regular RX messages (they are control-plane only). */
	const char* s = (const char*)&received_text[1];
	if (strncmp(s, "ACK ", 4) == 0) {
		unsigned int ack_seq = 0, ack_total = 0;
		if (sscanf(s + 4, " %u/%u", &ack_seq, &ack_total) == 2) {
			struct zigbee_data* ctxt = zigbee_ctxt();
			if (ctxt) {
				ctxt->last_ack_seq = (uint8_t)ack_seq;
				ESP_LOGD(TAG, "ACK received for chunk %u/%u", ack_seq, ack_total);
			}
		}
		return; /* do not enqueue ACKs */
	}

	/* Enqueue to RX queue for processing */
	zigbee_enqueue_rx_text((const char*)&received_text[1], text_len);

	/* If this looks like a chunk header (e.g. "1/3|...") -> queue an ACK
	 * to the coordinator so sender can progress. We queue an unacked send
	 * so the ACK is sent without expecting another ACK in return. */
	int header_len = 0;
	unsigned int seq = 0, total = 0;
	if (sscanf(s, "%u/%u|%n", &seq, &total, &header_len) == 2 && header_len > 0) {
		char ackbuf[32];
		snprintf(ackbuf, sizeof(ackbuf), "ACK %u/%u", seq, total);
		zigbee_queue_msg(ZB_MSG_SEND_UNACKED, ackbuf);
	}
}

/* Attribute-based handling removed: we use custom cluster command/char-string
 * payloads instead of ZCL attributes for text exchange. */

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
	if (size >= 1 && data[0] < size) {
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

	struct zigbee_data* ctxt = zigbee_ctxt();
	if (!ctxt) {
		ESP_LOGW(TAG, "No context, cannot send");
		return;
	}

	size_t text_len = strlen(text);
	if (text_len == 0)
		return;

	/* Ensure room for leading length byte and terminating NUL */
	if (text_len > (ZIGBEE_TEXT_MAX - 1)) {
		ESP_LOGW(TAG, "Text too long (%u bytes), truncating to %u", (unsigned)text_len, (unsigned)(ZIGBEE_TEXT_MAX - 1));
		text_len = (ZIGBEE_TEXT_MAX - 1);
	}

	/* Chunk payload into header/body pieces: "seq/total|body" */
	size_t body_cap = ZIGBEE_ZHA_CHUNK_BODY_LEN;
	uint16_t total = (text_len + body_cap - 1) / body_cap;

	for (uint16_t idx = 0; idx < total; ++idx) {
		uint16_t seq = idx + 1;
		size_t start = idx * body_cap;
		size_t this_body = (start + body_cap <= text_len) ? body_cap : (text_len - start);

		char chunk[ZIGBEE_TEXT_MAX + 1];
		int header_len = snprintf(chunk, sizeof(chunk), "%u/%u|", seq, total);
		if (header_len <= 0 || (size_t)header_len + this_body >= sizeof(chunk)) {
			ESP_LOGW(TAG, "Chunk header too large, aborting");
			return;
		}
		memcpy(&chunk[header_len], &text[start], this_body);
		chunk[header_len + this_body] = '\0';

		/* Build ZCL char-string local buffer */
		uint8_t local_sensor[ZIGBEE_TEXT_MAX + 1];
		size_t chunk_len = (size_t)header_len + this_body;
		local_sensor[0] = (uint8_t)chunk_len;
		memcpy(&local_sensor[1], chunk, chunk_len);
		local_sensor[1 + chunk_len] = '\0';

		bool acked = false;
		for (int attempt = 1; attempt <= 3 && !acked; ++attempt) {
			if (!esp_zb_lock_acquire(pdMS_TO_TICKS(1000))) {
				ESP_LOGW(TAG, "Failed to acquire Zigbee lock for chunk %u/%u (attempt %d)", seq, total, attempt);
				vTaskDelay(pdMS_TO_TICKS(50));
				continue;
			}

			esp_zb_zcl_custom_cluster_cmd_req_t cmd_req = {
				.zcl_basic_cmd = {
					.dst_addr_u.addr_short = 0x0000,
					.dst_endpoint = ZB_TEXT_ENDPOINT,
					.src_endpoint = ZB_TEXT_ENDPOINT,
				},
				.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
				.cluster_id = ZB_CLUSTER_RAMSES_RX,
				.profile_id = ESP_ZB_AF_HA_PROFILE_ID,
				.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
				.custom_cmd_id = 0x00,
				.data = {
					.type = ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
					.value = NULL,
					.size = (uint16_t)(chunk_len + 1),
				},
			};

			cmd_req.data.value = (void*)local_sensor;
			cmd_req.data.size = (uint16_t)(chunk_len + 1);
			uint8_t tsn = esp_zb_zcl_custom_cluster_cmd_req(&cmd_req);
			esp_zb_lock_release();

			if (tsn == 0xFF) {
				ESP_LOGW(TAG, "ZCL chunk send invalid TSN seq=%u total=%u attempt=%d", seq, total, attempt);
				vTaskDelay(pdMS_TO_TICKS(50));
				continue;
			}

			/* mark pending ack for this seq */
				/* clear any previous ACK state so stale acks don't match */
				ctxt->last_ack_seq = 0;
				ctxt->pending_ack_seq = (uint8_t)seq;
			ESP_LOGD(TAG, "ZCL chunk queued: %u/%u bytes=%u tsn=%u", seq, total, (unsigned)chunk_len, (unsigned)tsn);

			/* wait for application ACK */
			TickType_t start = xTaskGetTickCount();
			TickType_t timeout = pdMS_TO_TICKS(ZIGBEE_TX_CONFIRM_TIMEOUT_MS);
			while ((xTaskGetTickCount() - start) < timeout) {
				if (ctxt->last_ack_seq == (uint8_t)seq) {
					acked = true;
					ctxt->pending_ack_seq = 0;
					ESP_LOGD(TAG, "Received ACK for chunk %u/%u", seq, total);
					break;
				}
				vTaskDelay(pdMS_TO_TICKS(20));
			}

			if (!acked) {
				ESP_LOGW(TAG, "No ACK for chunk %u/%u attempt %d", seq, total, attempt);
			}
		}

		if (!acked) {
			ESP_LOGE(TAG, "Failed to send chunk %u/%u after retries, aborting message", seq, total);
			return;
		}

		if (seq < total) {
			vTaskDelay(pdMS_TO_TICKS(25));
		}
	}
}

/* Send a short text message without expecting an application-level ACK. */
static void zigbee_send_text_unacked(const char* text)
{
	if (!text || text[0] == '\0')
		return;

	size_t text_len = strlen(text);
	if (text_len > (ZIGBEE_TEXT_MAX - 1))
		text_len = (ZIGBEE_TEXT_MAX - 1);

	uint8_t local_sensor[ZIGBEE_TEXT_MAX + 1];
	local_sensor[0] = (uint8_t)text_len;
	memcpy(&local_sensor[1], text, text_len);
	local_sensor[text_len + 1] = '\0';

	if (!esp_zb_lock_acquire(pdMS_TO_TICKS(1000))) {
		ESP_LOGW(TAG, "Failed to acquire Zigbee lock for unacked send");
		return;
	}

	/* If this is an application ACK ("ACK seq/total") send it as a
	 * server command on the receive cluster (ZB_CLUSTER_TEXT_RECEIVE) so
	 * it matches the cluster the coordinator expects for responses. For
	 * other unacked payloads, use the send cluster (ZB_CLUSTER_TEXT_SEND).
	 */
	bool is_ack = false;
	if (text_len >= 4 && strncmp(text, "ACK ", 4) == 0) {
		is_ack = true;
	}

	esp_zb_zcl_custom_cluster_cmd_req_t cmd_req = {
		.zcl_basic_cmd = {
			.dst_addr_u.addr_short = 0x0000,
			.dst_endpoint = ZB_TEXT_ENDPOINT,
			.src_endpoint = ZB_TEXT_ENDPOINT,
		},
		.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
		.cluster_id = is_ack ? ZB_CLUSTER_RAMSES_TX : ZB_CLUSTER_RAMSES_RX,
		.profile_id = ESP_ZB_AF_HA_PROFILE_ID,
		.direction = is_ack ? ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI : ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
		.custom_cmd_id = is_ack ? 0x01 : 0x00,
		.data = {
			.type = ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
			.value = NULL,
			.size = (uint16_t)(text_len + 1),
		},
	};

	cmd_req.data.value = (void*)local_sensor;
	cmd_req.data.size = (uint16_t)(text_len + 1);
	uint8_t tsn = esp_zb_zcl_custom_cluster_cmd_req(&cmd_req);
	esp_zb_lock_release();

	if (tsn == 0xFF) {
		ESP_LOGW(TAG, "ZCL unacked send failed: invalid TSN");
	} else {
		ESP_LOGD(TAG, "ZCL unacked send queued: %u bytes tsn=%u (ack=%d)", (unsigned)text_len, (unsigned)tsn, is_ack);
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
		/* End device: start initialization only */
		esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
		break;
	case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
	case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
		if (err_status == ESP_OK) {
			if (zb_skip_autojoin == ZB_SKIP_AUTOJOIN_MAGIC) {
				/* Device was removed by coordinator in a previous session.
				 * Do NOT auto-start steering: wait for explicit user action. */
				zb_skip_autojoin = 0;
				ESP_LOGW(TAG, "Removed by coordinator: waiting for user to trigger pair (button or 'zigbee pair')");
				zigbee_set_state(ctxt, ZIGBEE_STATE_PAIRING);
			} else {
				/* Normal boot or user-initiated factory reset:
				 * auto-start steering. */
				zigbee_set_state(ctxt, ZIGBEE_STATE_PAIRING);
				esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
									   ESP_ZB_BDB_MODE_NETWORK_STEERING, 2000);
			}
		}
		break;
	case ESP_ZB_BDB_SIGNAL_STEERING:
		if (err_status == ESP_OK) {
			zigbee_set_state(ctxt, ZIGBEE_STATE_CONNECTED);
			ctxt->ready = true;
		} else {
			zigbee_set_state(ctxt, ZIGBEE_STATE_ERROR);
			esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
								   ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
		}
		break;
	case ESP_ZB_ZDO_SIGNAL_LEAVE:
		ctxt->ready = false;
		if (ctxt->factory_reset_pending) {
			/* Leave was triggered by our own factory reset request.
			 * ZHA has now removed the device from its database.
			 * Erase zb_storage and restart as a brand-new device. */
			ESP_LOGW(TAG, "Factory-reset Leave confirmed: erasing zb_storage and restarting");
			ctxt->factory_reset_pending = false;
			esp_zb_factory_reset();
			/* esp_zb_factory_reset() restarts — code below not reached */
		} else if (ctxt->state == ZIGBEE_STATE_CONNECTED) {
			/* Coordinator removed us while connected.
			 * Set the RTC skip flag so that any future boot (power-cycle)
			 * via DEVICE_REBOOT does NOT auto-start steering.
			 * No wipe or restart needed: ZBOSS has already cleared its
			 * internal network association (confirmed by zb_address_delete
			 * in the trace).  Just go to idle PAIRING state and wait for
			 * the user to explicitly trigger a re-pair. */
			ESP_LOGW(TAG, "Removed by coordinator: going idle, waiting for user to trigger pair");
			zb_skip_autojoin = ZB_SKIP_AUTOJOIN_MAGIC;
			zigbee_set_state(ctxt, ZIGBEE_STATE_PAIRING);
		} else {
			/* Leave during PAIRING/ERROR: authentication rejected
			 * (coordinator's permit-join is closed, or handshake failed
			 * on a freshly-wiped device).  Nothing to wipe; retry steering. */
			ESP_LOGW(TAG, "Auth rejected / transient Leave: retrying in 10s");
			zigbee_set_state(ctxt, ZIGBEE_STATE_PAIRING);
			esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
					   ESP_ZB_BDB_MODE_NETWORK_STEERING, 10000);
		}
		break;
	default:
		break;
	}
}

/* Button-driven factory-reset: hold FUNC for 5s to factory-reset when not pairing */
#define ZB_FACTORY_HOLD_MS (5000)

static struct zb_reset_ctx {
	StaticTimer_t timer_buf;
	TimerHandle_t hTimer;
	uint8_t expired;
} zb_reset_ctx;

static void zb_reset_timer_cb(TimerHandle_t xTimer)
{
	struct zb_reset_ctx* r = pvTimerGetTimerID(xTimer);
	if (!r->expired) {
		r->expired = 1;
		struct zigbee_data* ctxt = zigbee_ctxt();
		/* Notify the Zigbee app task — it will log and handle the reset.
		 * Do NOT call ESP_LOG here: the Timer Service stack (~2 KB) is too
		 * small for vfprintf and will overflow.  Use xTaskNotify (not the
		 * FromISR variant) because timer callbacks run in task context. */
		if (ctxt && ctxt->task) {
			xTaskNotify(ctxt->task, 1, eSetBits);
		}
		/* If no task yet, silently ignore — no logging from timer context. */
	}
}

static void enable_zb_reset(void)
{
	zb_reset_ctx.hTimer = xTimerCreateStatic("ZBReset", pdMS_TO_TICKS(ZB_FACTORY_HOLD_MS), pdFALSE, &zb_reset_ctx, zb_reset_timer_cb, &zb_reset_ctx.timer_buf);
	zb_reset_ctx.expired = 0;
}

/* Pairing LED blink: use a single 100ms periodic timer and a tick counter
 * to produce a 100ms ON every 1000ms (i.e. ON for 1 tick, OFF for 9 ticks).
 */
#define ZB_PAIR_TICK_MS (100)
#define ZB_PAIR_TICKS_PER_PERIOD (10) /* 10 * 100ms = 1000ms */

static struct pairing_blink {
	StaticTimer_t timer_buf;
	TimerHandle_t hTimer;
	uint8_t tick;
} pairing_blink;

static void pairing_blink_cb(TimerHandle_t xTimer)
{
	(void)xTimer;
	struct zigbee_data* ctxt = zigbee_ctxt();
	if (!ctxt || ctxt->state != ZIGBEE_STATE_PAIRING) {
		pairing_blink.tick = 0;
		led_off(LED_RX);
		return;
	}

	/* On tick 0 -> turn LED on for one tick, on tick 1..9 -> off */
	if (pairing_blink.tick == 0) {
		led_on(LED_RX);
	} else if (pairing_blink.tick == 1) {
		led_off(LED_RX);
	}

	pairing_blink.tick = (pairing_blink.tick + 1) % ZB_PAIR_TICKS_PER_PERIOD;
}

static void enable_pairing_blink(void)
{
	pairing_blink.tick = 0;
	pairing_blink.hTimer = xTimerCreateStatic("ZBPair", pdMS_TO_TICKS(ZB_PAIR_TICK_MS), pdTRUE, &pairing_blink, pairing_blink_cb, &pairing_blink.timer_buf);
}

static void zigbee_button_cb(struct button_event* event)
{
	static int8_t level = -1;

	if (!event)
		return;

	if (event->level != level) {
		if (event->level == 0) { // pressed
			zb_reset_ctx.expired = 0;
			if (zb_reset_ctx.hTimer)
				xTimerStart(zb_reset_ctx.hTimer, 0);
		} else { // released
			if (zb_reset_ctx.hTimer)
				xTimerStop(zb_reset_ctx.hTimer, 0);
			if (zb_reset_ctx.expired) {
				/* timer already fired and handled factory reset */
			}
		}
		level = event->level;
	}
}

static void Zigbee(void* param)
{
	struct zigbee_data* ctxt = param;

	ctxt->queue = xQueueCreate(ZIGBEE_QUEUE_LEN, sizeof(struct zigbee_msg));
	if (!ctxt->queue) {
		ESP_LOGE(TAG, "Failed to create TX queue!");
		vTaskDelete(NULL);
		return;
	}

	zigbee_rx_queue = xQueueCreate(ZIGBEE_RX_QUEUE_LEN, sizeof(struct zigbee_rx_msg));
	if (!zigbee_rx_queue) {
		ESP_LOGE(TAG, "Failed to create RX queue!");
		vTaskDelete(NULL);
		return;
	}

	for (;;) {
		/* Check for factory-reset notification from timer callback */
		uint32_t notify_val = 0;
		if (xTaskNotifyWait(0x00, 0xFFFFFFFF, &notify_val, 0) == pdTRUE) {
			if (notify_val & 1) {
				if (ctxt->state == ZIGBEE_STATE_CONNECTED) {
					/* Device is on a network: send ZDO Leave so ZHA cleanly removes
					 * the device, then SIGNAL_LEAVE → esp_zb_factory_reset(). */
					ESP_LOGI(TAG, "Factory reset requested: sending ZDO Leave to coordinator");
					ctxt->factory_reset_pending = true;
					if (esp_zb_lock_acquire(pdMS_TO_TICKS(2000))) {
						esp_zb_scheduler_alarm(
								(esp_zb_callback_t)esp_zb_bdb_reset_via_local_action, 0, 100);
						esp_zb_lock_release();
						ESP_LOGI(TAG, "Factory reset: ZDO Leave scheduled");
					} else {
						ESP_LOGW(TAG, "Failed to acquire Zigbee lock for factory reset");
						ctxt->factory_reset_pending = false;
					}
				} else {
					/* Not connected (e.g. waiting after coordinator removal).
					 * Just start steering — user wants to re-pair.
					 * esp_zb_factory_reset() would wipe storage (already empty)
					 * and restart, but simpler to just kick commissioning directly. */
					ESP_LOGI(TAG, "Pair requested: starting NETWORK_STEERING");
					zigbee_set_state(ctxt, ZIGBEE_STATE_PAIRING);
					if (esp_zb_lock_acquire(pdMS_TO_TICKS(2000))) {
						esp_zb_scheduler_alarm(
								(esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
								ESP_ZB_BDB_MODE_NETWORK_STEERING, 100);
						esp_zb_lock_release();
					}
				}
			}
		}
		/* Drain RX first (always) */
		struct zigbee_rx_msg rx_msg;
		while (xQueueReceive(zigbee_rx_queue, &rx_msg, 0) == pdTRUE) {
			ESP_LOGD(TAG, "Received Zigbee text: %s", rx_msg.text);
			if (!zigbee_handle_chunked_text(rx_msg.text)) {
				/* Non-chunked message - send to RF only, echo comes from serial RX */
				zigbee_tx_to_frame(rx_msg.text);
			}
		}

		/* Fire-and-forget TX: try to dequeue a TX message (short wait) and send immediately */
		struct zigbee_msg msg;
		BaseType_t res = xQueueReceive(ctxt->queue, &msg, pdMS_TO_TICKS(20));
		if (res == pdTRUE) {
			ESP_LOGD(TAG, "Dequeued msg type=%d ready=%d", msg.type, ctxt->ready);
			switch (msg.type) {
			case ZB_MSG_SENSOR_UPDATE:
				if (ctxt->ready) {
					ESP_LOGD(TAG, "Sending to Zigbee (fire-and-forget)");
					zigbee_send_text_chunked(msg.text);
				} else {
					ESP_LOGW(TAG, "Cannot send, not ready");
				}
				break;
			case ZB_MSG_SEND_UNACKED:
				if (ctxt->ready) {
					ESP_LOGD(TAG, "Sending unacked to Zigbee");
					/* send directly without expecting application ACKs */
					zigbee_send_text_unacked(msg.text);
				} else {
					ESP_LOGW(TAG, "Cannot send unacked, not ready");
				}
				break;
			case ZB_MSG_TX_TO_FRAME:
				if (!zigbee_handle_chunked_text(msg.text)) {
					zigbee_tx_to_frame(msg.text);
				}
				break;
			default:
				break;
			}
		}
		vTaskDelay(1);
	}
}

static void zigbee_stack_task(void* param)
{
	(void)param;
	struct zigbee_data* ctxt = zigbee_ctxt();
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
	/* Cluster 0xFC00 as CLIENT (output) - RAMSES RX: ESP -> coordinator */
	esp_zb_attribute_list_t* text_send_cluster = esp_zb_zcl_attr_list_create(ZB_CLUSTER_RAMSES_RX);
	esp_zb_cluster_list_add_custom_cluster(cluster_list, text_send_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

	/* Cluster 0xFC01 as SERVER (input) - RAMSES TX: coordinator -> ESP */
	esp_zb_attribute_list_t* text_recv_cluster = esp_zb_zcl_attr_list_create(ZB_CLUSTER_RAMSES_TX);
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

	/* Enable button-driven factory-reset and register callback */
	enable_zb_reset();
	button_register(BUTTON_FUNC, zigbee_button_cb);

	/* Enable pairing LED blink (timer created here) */
	enable_pairing_blink();
	if (pairing_blink.hTimer)
		xTimerStart(pairing_blink.hTimer, 0);

	ESP_ERROR_CHECK(esp_zb_start(false));

	esp_zb_stack_main_loop();
}

void ramses_zigbee_init(BaseType_t coreID)
{
	struct zigbee_data* ctxt = zigbee_ctxt();
	ctxt->coreID = coreID;

	/* Register console commands for Zigbee control */
#include "zigbee_cmd.h"
	zigbee_register();
	xTaskCreatePinnedToCore(zigbee_stack_task, "ZigbeeStack", 16384, NULL, 10, &ctxt->stack_task, ctxt->coreID);
	xTaskCreatePinnedToCore(Zigbee, "Zigbee", 16384, ctxt, 5, &ctxt->task, ctxt->coreID);
}

void zigbee_update_sensor_text(const char* text)
{
	zigbee_queue_msg(ZB_MSG_SENSOR_UPDATE, text);
}

void zigbee_receive_text(const char* text)
{
	zigbee_queue_msg(ZB_MSG_TX_TO_FRAME, text);
}

/* Request a factory reset from any task.  Posts to the Zigbee app task so the
 * actual ZBOSS scheduler call happens in task context with the Zigbee lock held. */
void zigbee_request_factory_reset(void)
{
	struct zigbee_data* ctxt = zigbee_ctxt();
	if (!ctxt || !ctxt->task) {
		ESP_LOGW(TAG, "zigbee_request_factory_reset: task not ready");
		return;
	}
	xTaskNotify(ctxt->task, 1, eSetBits);
}

zigbee_state_t zigbee_get_state(void)
{
	struct zigbee_data* ctxt = zigbee_ctxt();
	return ctxt ? ctxt->state : ZIGBEE_STATE_ERROR;
}
