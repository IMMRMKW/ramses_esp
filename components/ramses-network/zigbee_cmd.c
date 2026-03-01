/********************************************************************
 * ramses_esp
 * zigbee_cmd.c
 *
 * Zigbee console commands
 */
#include "cmd.h"
#include "zigbee.h"
#include "zigbee_cmd.h"

#include "esp_err.h"
#include "esp_zigbee_core.h"

static int zigbee_cmd_pair(int argc, char** argv)
{
    /* Trigger through the Zigbee task (same path as the button hold).
     * If connected: send ZDO Leave, erase NVRAM, then re-pair.
     * If not connected (e.g. waiting after coordinator removal): start steering. */
    zigbee_request_factory_reset();
    return 0;
}

static esp_console_cmd_t const zigbee_cmds[] = {
    {
        .command = "pair",
        .help = "Pair with a Zigbee network (factory-resets first if already connected)",
        .hint = NULL,
        .func = &zigbee_cmd_pair,
    },
    { NULL_COMMAND }
};

static int zigbee_cmd(int argc, char** argv)
{
    return cmd_menu(argc, argv, zigbee_cmds, argv[0]);
}

void zigbee_register(void)
{
    const esp_console_cmd_t zcmd[] = {
        {
            .command = "zigbee",
            .help = "Zigbee commands, enter 'zigbee' for list",
            .hint = NULL,
            .func = &zigbee_cmd,
        },
        { NULL_COMMAND }
    };

    cmd_menu_register(zcmd);
}
