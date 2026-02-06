/********************************************************************
 * ramses_esp
 * zigbee.h
 *
 * (C) 2026
 *
 * Zigbee bridge interface (placeholder state machine)
 *
 */
#ifndef _ZIGBEE_H_
#define _ZIGBEE_H_

#include "freertos/FreeRTOS.h"

typedef enum {
	ZIGBEE_STATE_IDLE = 0,
	ZIGBEE_STATE_PAIRING,
	ZIGBEE_STATE_CONNECTED,
	ZIGBEE_STATE_ERROR,
} zigbee_state_t;

// Initialize Zigbee task/state machine
extern void ramses_zigbee_init(BaseType_t coreID);

// Feed gateway RX text to Zigbee sensor update path
extern void zigbee_update_sensor_text(const char* text);

// Feed HA inbound text to CC1101 frame path
extern void zigbee_receive_text(const char* text);

// Read current Zigbee state
extern zigbee_state_t zigbee_get_state(void);

#endif // _ZIGBEE_H_
