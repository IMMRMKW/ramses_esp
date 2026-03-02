/********************************************************************
 * ramses_esp
 * network_cmd.h
 *
 * (C) 2023 Peter Price
 *
 * General Network Commands
 *
 */
#include "cmd.h"
#include "network_cmd.h"

#include "esp_system.h"
#include "esp_log.h"
#include "ramses_network.h"

static const char* TAG = "NETWORK_CMD";

/*********************************************************
 * MQTT Server commands
 */

static int mqtt_cmd_broker( int argc, char **argv ) {

  if( argc>1 )
	  NET_set_mqtt_broker( argv[1] );

  return 0;
}

static int mqtt_cmd_user( int argc, char **argv ) {

  if( argc>1 )
	  NET_set_mqtt_user( argv[1] );

  return 0;
}

static int mqtt_cmd_password( int argc, char **argv ) {

  if( argc>1 )
	  NET_set_mqtt_password( argv[1] );

  return 0;
}

static esp_console_cmd_t const mqtt_cmds[] = {
  {
    .command = "broker",
    .help = "Set MQTT <Broker>",
    .hint = NULL,
    .func = &mqtt_cmd_broker,
  },
  {
    .command = "user",
    .help = "Set MQTT <User>",
    .hint = NULL,
    .func = &mqtt_cmd_user,
  },
  {
    .command = "password",
    .help = "Set MQTT <Password>",
    .hint = NULL,
    .func = &mqtt_cmd_password,
  },
  // List termination
  { NULL_COMMAND }
};

static int mqtt_cmd( int argc, char **argv ) {
  return cmd_menu( argc, argv, mqtt_cmds, argv[0] );
}

/*********************************************************
 * SNTP Server commands
 */

static int sntp_cmd_server( int argc, char **argv ) {

  if( argc>1 )
	  NET_set_sntp_server( argv[1] );

  return 0;
}

static esp_console_cmd_t const sntp_cmds[] = {
  {
    .command = "server",
    .help = "Set SNTP <Server>",
    .hint = NULL,
    .func = &sntp_cmd_server,
  },
  // List termination
  { NULL_COMMAND }
};

static int sntp_cmd( int argc, char **argv ) {
  return cmd_menu( argc, argv, sntp_cmds, argv[0] );
}

/*********************************************************
 * Timezone
 */

static int timezone_cmd( int argc, char **argv ) {

  if( argc>1 )
    NET_set_timezone( argv[1] );
  else
	NET_show_timezones();

  return 0;
}

/*********************************************************
 * Network mode commands
 */

static int net_cmd_mode_show(int argc, char** argv)
{
    printf("# Network mode: %s\n", NET_get_mode());
    return 0;
}

static int net_cmd_mode_zigbee(int argc, char** argv)
{
    ESP_LOGI(TAG, "Setting network mode to zigbee, restarting...");
    NET_set_mode("zigbee");
    esp_restart();
    return 0;  /* unreachable */
}

static int net_cmd_mode_wifi(int argc, char** argv)
{
    ESP_LOGI(TAG, "Setting network mode to wifi, restarting...");
    NET_set_mode("wifi");
    esp_restart();
    return 0;  /* unreachable */
}

static esp_console_cmd_t const network_mode_cmds[] = {
    {
        .command = "zigbee",
        .help = "Switch to Zigbee mode (saves to NVS and restarts)",
        .hint = NULL,
        .func = &net_cmd_mode_zigbee,
    },
    {
        .command = "wifi",
        .help = "Switch to WiFi+MQTT mode (saves to NVS and restarts)",
        .hint = NULL,
        .func = &net_cmd_mode_wifi,
    },
    {
        .command = "show",
        .help = "Show current network mode",
        .hint = NULL,
        .func = &net_cmd_mode_show,
    },
    { NULL_COMMAND }
};

static int net_cmd_network(int argc, char** argv)
{
    return cmd_menu(argc, argv, network_mode_cmds, argv[0]);
}

/*********************************************************
 * Top Level commands
 */

void network_register(void) {
  static esp_console_cmd_t const net_cmds[] = {
    {
      .command = "network",
      .help = "Network mode commands, enter 'network' for list",
      .hint = NULL,
      .func = &net_cmd_network,
    },
    {
      .command = "mqtt",
      .help = "MQTT commands, enter 'mqtt' for list",
      .hint = NULL,
      .func = &mqtt_cmd,
    },
    {
      .command = "sntp",
      .help = "SNTP commands, enter 'sntp' for list",
      .hint = NULL,
      .func = &sntp_cmd,
    },
    {
      .command = "timezone",
      .help = "timezone <tz string>",
      .hint = NULL,
      .func = &timezone_cmd,
    },
    // List termination
    { NULL_COMMAND }
  };

  cmd_menu_register( net_cmds );
}
