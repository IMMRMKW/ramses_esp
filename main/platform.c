/*
 * platform.c
 *
 *  Created on: 15 Jan 2025
 *      Author: peter
 */
#include "driver/gpio.h"
#include "stdio.h"

#include "platform.h"

#define GPIO_PIN_SEL(_p) (1ULL << (_p))

#define PIN_MAX 1
static const struct gpio_map {
    uint32_t gpio;
} map[PIN_MAX] = {
    { CONFIG_CS_PLATFORM_PIN },
};

static uint8_t const platforms[] = {
    /* 0x00 */ PLATFORM_GW,
    /* 0x01 */ PLATFORM_GW + PLATFORM_CS,
};

static void platform_init(void)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 0,
        .pull_down_en = 0,
        .pull_up_en = 1
    };

    uint8_t pin;
    for (pin = 0; pin < PIN_MAX; pin++) {
        io_conf.pin_bit_mask = GPIO_PIN_SEL(map[pin].gpio);
        gpio_config(&io_conf);
    }
}

uint8_t platform(void)
{
    uint8_t pins = 0;
    uint8_t pin;

    platform_init();

    for (pin = 0; pin < PIN_MAX; pin++) {
        if (gpio_get_level(map[pin].gpio) == 0) {
            pins |= 1 << pin;
        }
    }

    if (pins >= sizeof(platforms))
        pins = 0; // Ddefault to GW only

    return platforms[pins];
}
