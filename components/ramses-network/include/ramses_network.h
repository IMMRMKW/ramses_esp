/********************************************************************
 * ramses_esp
 * ramses_network.h
 *
 * (C) 2023 Peter Price
 *
 * Network
 *
 */
#ifndef _RAMSES_NETWORK_H_
#define _RAMSES_NETWORK_H_

#include "freertos/FreeRTOS.h"

extern void NET_set_mqtt_broker( char *new_server );
extern void NET_set_mqtt_user( char *new_user );
extern void NET_set_mqtt_password( char *new_password );

extern char const *NET_get_mqtt_broker(void);
extern char const *NET_get_mqtt_user(void);
extern char const *NET_get_mqtt_password(void);

extern void NET_set_sntp_server( char *new_server );
extern void NET_set_timezone( char *new_timezone );
extern void NET_show_timezones( void);

/* Network transport mode: "zigbee" (default) or "wifi".
 * Changes are persisted to NVS and take effect after esp_restart(). */
extern void        NET_set_mode( const char *mode );
extern const char *NET_get_mode( void );

extern void ramses_network_init( BaseType_t coreID );

#endif // _RAMSES_NETWORK_H_
