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
#include "esp_system.h"
#include "esp_random.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "esp_attr.h"   /* RTC_NOINIT_ATTR */

#include "ha/esp_zigbee_ha_standard.h"
#include "esp_zigbee_core.h"
#include "aps/esp_zigbee_aps.h"
#include <ramses_buttons.h>
#include "ramses_led.h"

#include "message.h"
#include "zigbee.h"

#define ZIGBEE_TEXT_MAX 256
#define ZIGBEE_QUEUE_LEN 100  /* Increased to handle RF message bursts without drops */
#define ZIGBEE_RX_QUEUE_LEN 8
#define ZIGBEE_ZHA_CHUNK_BODY_LEN 32  /* Reduced from 54 to prevent APS fragmentation */
#define ZIGBEE_TX_CONFIRM_TIMEOUT_MS 30000 /* Allow stack-owned APS retries to finish. */
#define ZIGBEE_CHUNK_TIMEOUT_MS 35000
#define ZIGBEE_NATIVE_CHUNK_LEN 30
#define ZIGBEE_NATIVE_CMD 0x02
#define ZIGBEE_NATIVE_MODE_CMD 0x03

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
#define ZB_NATIVE_WATCHDOG_MAGIC 0x41505332
RTC_NOINIT_ATTR static uint32_t zb_native_watchdog_mode;

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
	bool native;
	uint32_t message_id;
	uint8_t seq;
	uint8_t total;
	char text[ZIGBEE_TEXT_MAX];
	size_t len;
};

static QueueHandle_t zigbee_rx_queue = NULL;

struct zigbee_chunk_state {
	bool active;
	uint8_t total;
	uint8_t next_seq;
	size_t length;
	TickType_t received_at;
	char buffer[ZIGBEE_TEXT_MAX + 1];
};

static struct zigbee_chunk_state chunk_state = {0};
static struct zigbee_chunk_state native_chunk_state = {0};
static uint32_t native_rx_message_id;
static uint32_t native_rx_completed_id;
static bool native_rx_completed;

static void zigbee_handle_native_chunk(const struct zigbee_rx_msg* msg);


enum zigbee_msg_type {
	ZB_MSG_SENSOR_UPDATE,
	ZB_MSG_TX_TO_FRAME,
};

static void zigbee_queue_msg(enum zigbee_msg_type type, const char* text);

static bool zigbee_handle_chunked_text(const char* text);
static void zigbee_tx_to_frame(const char* text);
static void zigbee_send_text_chunked(const char* text);
static void zigbee_drain_rx(void);
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

	if (total == 0 || total > 8 || seq == 0 || seq > total) {
		ESP_LOGW(TAG, "Zigbee chunk invalid seq=%u total=%u", seq, total);
		zigbee_chunk_reset();
		return true;
	}

	const char* body = text + header_len;
	size_t body_len = strlen(body);
	if (body_len == 0) {
		ESP_LOGW(TAG, "Zigbee chunk %u/%u has empty body", seq, total);
		zigbee_chunk_reset();
		return true;
	}

	if (chunk_state.active && xTaskGetTickCount() - chunk_state.received_at >= pdMS_TO_TICKS(ZIGBEE_CHUNK_TIMEOUT_MS))
		zigbee_chunk_reset();

	if (!chunk_state.active || seq == 1) {
		zigbee_chunk_reset();
		chunk_state.active = true;
		chunk_state.total = (uint8_t)total;
		chunk_state.next_seq = 1;
	}

	if (chunk_state.total != total || chunk_state.next_seq != seq) {
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
	chunk_state.received_at = xTaskGetTickCount();

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
	bool native_mode; /* Set by the coordinator; access under the Zigbee lock. */
	uint32_t native_message_id;
	bool tx_busy; /* Access only with the Zigbee lock held. */
	uint8_t tx_tsn;
	uint8_t tx_asdu[ZIGBEE_ZHA_CHUNK_BODY_LEN + 12];
	size_t tx_asdu_len;
	QueueHandle_t tx_confirm_queue;

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
	if (strnlen(text, ZIGBEE_TEXT_MAX) >= ZIGBEE_TEXT_MAX) {
		if (!xPortInIsrContext())
			ESP_LOGW(TAG, "Dropping oversized Zigbee message before enqueue");
		return;
	}

	struct zigbee_msg msg = {
		.type = type,
	};
	strncpy(msg.text, text, sizeof(msg.text) - 1);
	msg.text[sizeof(msg.text) - 1] = '\0';

	ESP_LOGD(TAG, "Queuing msg type=%d ready=%d text=%s", type, ctxt->ready, text);

	if (xPortInIsrContext()) {
		BaseType_t higher_priority_woken = pdFALSE;
		xQueueSendFromISR(ctxt->queue, &msg, &higher_priority_woken);
	} else {
		if (xQueueSend(ctxt->queue, &msg, 0) != pdTRUE)
			ESP_LOGW(TAG, "Zigbee TX queue full, dropping message");
	}
}

static void zigbee_enqueue_rx_text(const char* text, size_t len)
{
	if (!text || len == 0 || !zigbee_rx_queue)
		return;
	if (len > ZIGBEE_TEXT_MAX - 1)
		len = ZIGBEE_TEXT_MAX - 1;

	struct zigbee_rx_msg rx_msg = {0};
	memcpy(rx_msg.text, text, len);
	rx_msg.text[len] = '\0';
	rx_msg.len = len;

	if (xPortInIsrContext()) {
		BaseType_t higher_priority_woken = pdFALSE;
		xQueueSendFromISR(zigbee_rx_queue, &rx_msg, &higher_priority_woken);
	} else {
		if (xQueueSend(zigbee_rx_queue, &rx_msg, 0) != pdTRUE)
			ESP_LOGW(TAG, "Zigbee RX queue full, dropping command");
	}
}


static struct zigbee_data* zigbee_ctxt(void)
{
	static struct zigbee_data zigbee = {
		.state = ZIGBEE_STATE_IDLE,
		.ready = false,
		.tx_busy = false,
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
		if (new_state == ZIGBEE_STATE_CONNECTED)
			printf("# ZIGBEE: Connected\n");
		else if (ctxt->state == ZIGBEE_STATE_CONNECTED)
			printf("# ZIGBEE: Disconnected\n");
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

	/* Ignore legacy control messages; APS now owns delivery acknowledgement. */
	const char* text = (const char*)&received_text[1];
	if (strncmp(text, "ACK ", 4) != 0)
		zigbee_enqueue_rx_text(text, text_len);
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

	if (message->info.cluster == ZB_CLUSTER_RAMSES_TX &&
		message->info.command.id == ZIGBEE_NATIVE_MODE_CMD) {
		if (size != 1 || data[0] > 1)
			return ESP_ERR_INVALID_ARG;
		zigbee_ctxt()->native_mode = data[0] != 0;
		return ESP_OK;
	}
	if (message->info.cluster == ZB_CLUSTER_RAMSES_TX &&
		message->info.command.id == ZIGBEE_NATIVE_CMD) {
		/* uint32 message ID (little-endian), seq, total, char-string. */
		if (size < 8 || data[6] == 0 || data[6] > ZIGBEE_NATIVE_CHUNK_LEN ||
			size != 7 + data[6] || data[4] == 0 || data[4] > data[5] || data[5] > 9)
			return ESP_ERR_INVALID_ARG;
		struct zigbee_rx_msg rx = {
			.native = true,
			.message_id = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
				((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24),
			.seq = data[4], .total = data[5], .len = data[6],
		};
		for (size_t i = 0; i < rx.len; ++i) {
			if (data[7 + i] == 0 || data[7 + i] > 0x7f)
				return ESP_ERR_INVALID_ARG;
		}
		memcpy(rx.text, data + 7, rx.len);
		if (!zigbee_rx_queue || xQueueSend(zigbee_rx_queue, &rx, 0) != pdTRUE) {
			ESP_LOGW(TAG, "Native Zigbee RX queue full, dropping chunk %u/%u", rx.seq, rx.total);
			return ESP_ERR_NO_MEM;
		}
		zigbee_ctxt()->native_mode = true;
		return ESP_OK;
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

/* The stack invokes this callback under its lock. Match the complete ASDU
 * and endpoints, not merely a sequence number which can wrap. Other stack
 * traffic must never release our single in-flight buffer. */
static void zigbee_aps_confirm(esp_zb_apsde_data_confirm_t confirm)
{
	struct zigbee_data* ctxt = zigbee_ctxt();
	if (!ctxt->tx_busy || confirm.dst_addr_mode != ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT ||
		confirm.dst_addr.addr_short != 0x0000 || confirm.src_endpoint != ZB_TEXT_ENDPOINT ||
		confirm.dst_endpoint != ZB_TEXT_ENDPOINT || !confirm.asdu ||
		confirm.asdu_length != ctxt->tx_asdu_len ||
		memcmp(confirm.asdu, ctxt->tx_asdu, ctxt->tx_asdu_len) != 0)
		return;

	ctxt->tx_busy = false;
	xQueueOverwrite(ctxt->tx_confirm_queue, &confirm.status);
}

static void zigbee_send_text_chunked(const char* text)
{
	if (!text || !*text)
		return;
	struct zigbee_data* ctxt = zigbee_ctxt();
	size_t text_len = strlen(text);
	if (text_len >= ZIGBEE_TEXT_MAX) {
		ESP_LOGW(TAG, "Dropping oversized Zigbee message (%u bytes)", (unsigned)text_len);
		return;
	}
	if (!esp_zb_lock_acquire(pdMS_TO_TICKS(1000)))
		return;
	bool native = ctxt->native_mode;
	uint32_t message_id = ++ctxt->native_message_id;
	esp_zb_lock_release();
	size_t body_cap = native ? ZIGBEE_NATIVE_CHUNK_LEN : ZIGBEE_ZHA_CHUNK_BODY_LEN;
	uint16_t total = (text_len + body_cap - 1) / body_cap;
	for (uint16_t seq = 1; seq <= total; ++seq) {
		size_t start = (seq - 1) * body_cap;
		size_t body_len = text_len - start;
		if (body_len > body_cap)
			body_len = body_cap;

		if (!esp_zb_lock_acquire(pdMS_TO_TICKS(1000))) {
			ESP_LOGW(TAG, "Cannot acquire Zigbee lock, aborting message");
			return;
		}
		if (ctxt->tx_busy) {
			esp_zb_lock_release();
			ESP_LOGW(TAG, "Previous APS request still pending, dropping message");
			return;
		}
		xQueueReset(ctxt->tx_confirm_queue);
		/* Native command 2 is server-to-client, matching the quirk's
		 * ClientCommandDefs. Keep the old command 0 direction unchanged. */
		ctxt->tx_asdu[0] = native ? 0x19 : 0x11;
		ctxt->tx_asdu[1] = ctxt->tx_tsn++;
		ctxt->tx_asdu[2] = native ? ZIGBEE_NATIVE_CMD : 0x00;
		if (native) {
			for (unsigned i = 0; i < 4; ++i)
				ctxt->tx_asdu[3 + i] = message_id >> (8 * i);
			ctxt->tx_asdu[7] = seq;
			ctxt->tx_asdu[8] = total;
			ctxt->tx_asdu[9] = body_len;
			memcpy(ctxt->tx_asdu + 10, text + start, body_len);
			ctxt->tx_asdu_len = 10 + body_len;
		} else {
			int header_len = snprintf((char*)&ctxt->tx_asdu[4], sizeof(ctxt->tx_asdu) - 4,
					"%u/%u|", seq, total);
			memcpy(&ctxt->tx_asdu[4 + header_len], text + start, body_len);
			ctxt->tx_asdu[3] = header_len + body_len;
			ctxt->tx_asdu_len = 4 + header_len + body_len;
		}
		esp_zb_apsde_data_req_t req = {
			.dst_addr_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
			.dst_addr.addr_short = 0x0000,
			.dst_endpoint = ZB_TEXT_ENDPOINT,
			.src_endpoint = ZB_TEXT_ENDPOINT,
			.profile_id = ESP_ZB_AF_HA_PROFILE_ID,
			.cluster_id = ZB_CLUSTER_RAMSES_RX,
			.asdu = ctxt->tx_asdu,
			.asdu_length = ctxt->tx_asdu_len,
			.tx_options = ESP_ZB_APSDE_TX_OPT_ACK_TX,
		};
		/* Arm before submitting: a fast confirm must not be lost. */
		ctxt->tx_busy = true;
		esp_err_t err = esp_zb_aps_data_request(&req);
		if (err != ESP_OK)
			ctxt->tx_busy = false;
		esp_zb_lock_release();
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "APS submit failed for chunk %u/%u: %s", seq, total, esp_err_to_name(err));
			return;
		}

		TickType_t sent_at = xTaskGetTickCount();
		uint8_t status;
		for (;;) {
			/* Keep coordinator-to-RF commands moving during APS retries. */
			zigbee_drain_rx();
			if (xQueueReceive(ctxt->tx_confirm_queue, &status, pdMS_TO_TICKS(20)) == pdTRUE)
				break;
			if (xTaskGetTickCount() - sent_at >= pdMS_TO_TICKS(ZIGBEE_TX_CONFIRM_TIMEOUT_MS)) {
				/* The stack has failed to release its in-flight request.
				 * Reboot to reclaim buffers rather than opening the gate
				 * unsafely or remaining permanently blocked. */
				ESP_LOGE(TAG, "APS confirm watchdog expired for chunk %u/%u; restarting", seq, total);
				/* Preserve the negotiated wire format across this recovery. */
				zb_native_watchdog_mode = native ? ZB_NATIVE_WATCHDOG_MAGIC : 0;
				esp_restart();
				return;
			}
		}
		if (status != 0) {
			ESP_LOGW(TAG, "APS delivery failed for chunk %u/%u: 0x%02x", seq, total, status);
			return; /* Stack owns retries; never blindly replay an RF command. */
		}
	}
}

static void zigbee_drain_rx(void)
{
	struct zigbee_rx_msg rx_msg;
	while (xQueueReceive(zigbee_rx_queue, &rx_msg, 0) == pdTRUE) {
		if (rx_msg.native) {
			zigbee_handle_native_chunk(&rx_msg);
			continue;
		}
		if (!zigbee_handle_chunked_text(rx_msg.text))
			zigbee_tx_to_frame(rx_msg.text);
	}
}

static void zigbee_handle_native_chunk(const struct zigbee_rx_msg* msg)
{
	struct zigbee_chunk_state* state = &native_chunk_state;
	if (native_rx_completed && msg->message_id == native_rx_completed_id)
		return;
	if (state->active && xTaskGetTickCount() - state->received_at >= pdMS_TO_TICKS(ZIGBEE_CHUNK_TIMEOUT_MS))
		state->active = false;
	if (msg->seq == 1 && (!state->active || native_rx_message_id != msg->message_id)) {
		memset(state, 0, sizeof(*state));
		state->active = true;
		state->total = msg->total;
		state->next_seq = 1;
		native_rx_message_id = msg->message_id;
	}
	if (!state->active || native_rx_message_id != msg->message_id)
		return;
	if (msg->seq < state->next_seq)
		return; /* Already accepted: do not execute duplicate RF commands. */
	if (msg->total != state->total || msg->seq != state->next_seq || state->length + msg->len >= ZIGBEE_TEXT_MAX) {
		ESP_LOGW(TAG, "Dropping incomplete native Zigbee message %08lx", (unsigned long)msg->message_id);
		state->active = false;
		return;
	}
	memcpy(state->buffer + state->length, msg->text, msg->len);
	state->length += msg->len;
	state->buffer[state->length] = 0;
	state->next_seq++;
	state->received_at = xTaskGetTickCount();
	if (msg->seq == msg->total) {
		native_rx_completed_id = msg->message_id;
		native_rx_completed = true;
		state->active = false;
		zigbee_tx_to_frame(state->buffer);
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
		zigbee_drain_rx();

		/* Send one message at a time, paced by APS delivery confirms. */
		struct zigbee_msg msg;
		BaseType_t res = xQueueReceive(ctxt->queue, &msg, pdMS_TO_TICKS(20));
		if (res == pdTRUE) {
			ESP_LOGD(TAG, "Dequeued msg type=%d ready=%d", msg.type, ctxt->ready);
			switch (msg.type) {
			case ZB_MSG_SENSOR_UPDATE:
				if (ctxt->ready) {
					ESP_LOGD(TAG, "Sending to Zigbee (APS acknowledged)");
					zigbee_send_text_chunked(msg.text);
				} else {
					ESP_LOGW(TAG, "Cannot send, not ready");
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
	esp_zb_aps_data_confirm_handler_register(zigbee_aps_confirm);
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
	ctxt->native_message_id = esp_random();
	ctxt->native_mode = esp_reset_reason() == ESP_RST_SW && zb_native_watchdog_mode == ZB_NATIVE_WATCHDOG_MAGIC;
	zb_native_watchdog_mode = 0;
	ctxt->tx_confirm_queue = xQueueCreate(1, sizeof(uint8_t));
	if (!ctxt->tx_confirm_queue) {
		ESP_LOGE(TAG, "Cannot create APS confirm queue");
		return;
	}

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
