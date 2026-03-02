# RAMSES ESP

I forked the [RAMSES_ESP](https://github.com/IndaloTech/ramses_esp/) project of [IndaloTech](https://github.com/IndaloTech) such that I could make a version which would also work with the ESP32-C6 (or other single-core ESPs), built with [ESP-IDF](https://docs.espressif.com/projects/esp-idf/). If you would like to use my board, instructions for using it are found [here](https://github.com/IMMRMKW/ramses_esp/wiki).

Improvements that I made:
  - In command, the console_readline had a bug that saved the '/n' character.
  - In command, the cmd_work had a bug that would continuously print the inserted command if it was not recognized.
  - The code can now be compiled for the IDF_TARGET_ESP32C6, most config files were changed to accomodate this.
  - The SNTP
  - In ramses_network, the net_timezone function missed the tzset() function.
  - Most notably, several changes were made to cc1101 and frame.
    - cc1101 now uses CC_IOCFG0 = 0x03. This makes the GDO0 only goes high once the buffer is full, and low at CC_FIFOTHR, which is now set at 17. As a result, the ESP32 will have much more time between the interrupts given by the CC1101.
    - Also, the ESP32 will now wait until the entire message is sent, and there is a time-out in case the sending process takes too long. This ensures the CC1101 will never go to idle mode, and if this somehow still happens, the ESP32 can recover from this situation.
    - Changed CC_PKTLEN to 0x3D. The CC1101 suffers from a known bug that if in RX mode the FIFO is full, the chip hangs. I am not sure if the code ever suffered from this, but I changed it regardlessly.
  - It should now be possible to upload the software to both single and multi-core ESPs. (But I only tested this on the ESP32-C6, feel free to try this with other processors)

## Support
Buy me a coffee or purchase the board that I made for this in my [shop](https://elecram.com/products/esp32-c6-wifi-zigbee-to-855-925mhz-wireless?variant=52605637099853) or on [Tindie](https://www.tindie.com/products/elecram/esp32-c6-wifizigbee-to-855-925mhz-wireless/).

<a href="https://www.buymeacoffee.com/immrmkw" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/default-orange.png" alt="Buy Me A Coffee" height="41" width="174"></a>
