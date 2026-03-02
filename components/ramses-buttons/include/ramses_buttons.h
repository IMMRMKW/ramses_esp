/********************************************************************
 * ramses_esp
 * ramses_buttons.h
 *
 * (C) 2025 Peter Price
 *
 * Monitor buttons
 *
 */
#ifndef _RAMSES_BUTTONS_H_
#define _RAMSES_BUTTONS_H_

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sdkconfig.h"

#ifndef CONFIG_RAMSES_BUTTON_GPIO
#define CONFIG_RAMSES_BUTTON_GPIO 9
#endif

#define _BUTTON_LIST \
  _BUTTON(BUTTON_FUNC, "FUNC", CONFIG_RAMSES_BUTTON_GPIO ) \

#define _BUTTON( _e, _t, _i ) _e,
enum buttons{ BUTTON_NONE, _BUTTON_LIST BUTTON_MAX };
#undef _BUTTON

struct button_event {
  TickType_t ticks;
  uint8_t gpio;
  uint8_t level;
} ;

typedef void (*button_cb)( struct button_event *event );
extern void button_register( enum buttons button, button_cb cb );

extern void ramses_buttons_init(BaseType_t coreID);

#endif /* _RAMSES_BUTTONS_H_ */
