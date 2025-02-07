/********************************************************************
 * ramses_esp
 * main.c
 *
 * (C) 2023 Peter Price
 *
 * ramses_esp is designed to run on a two core ESP32-S3
 *
 * The first core is used for the radio interface to the cc1101.
 * This core handles the RX and TX of RAMSES messages from the radio.
 *
 * The second core is responsible for communications with a host system.
 * It can implement a variety of protocols to transfer the message contents
 * between the ESP32 device and the host.
 */
#include <stdio.h>
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_intr_alloc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"

#include "ramses_debug.h"
#include "ramses_led.h"
#include "ramses_buttons.h"
#include "ramses_nvs.h"
#include "ramses_network.h"

#include "platform.h"
#include "radio.h"
#include "host.h"

void app_main(void)
{
  const esp_app_desc_t *app = esp_app_get_description();
  printf("# %s %s\n",app->project_name, app->version );

  uint8_t platforms = platform();
  if( platforms!=PLATFORM_GW )  // Don't change behaviour of pure gateway device'
    printf("# %02x\n", platforms );

  ESP_ERROR_CHECK( gpio_install_isr_service(0) ) ;
  ESP_ERROR_CHECK( esp_event_loop_create_default() );

  ramses_debug_init();
  ramses_led_init();
  ramses_buttons_init( CONFIG_HOST_CORE );
  ramses_nvs_init();
  ramses_network_init( CONFIG_HOST_CORE );

  Radio_init( CONFIG_RADIO_CORE );
  Host_init( CONFIG_HOST_CORE, platforms );
}
