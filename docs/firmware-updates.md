# Firmware updates through Home Assistant

The implementation is on the development branches, not yet on main or in a published release. A hardware OTA test is required before publishing it. The catalogue URLs below become available when a release containing this workflow is published.

GitHub Releases hosts every file. A separate website is unnecessary. Latest-release catalogue URLs are stable, while each catalogue pins firmware downloads to a specific release tag.

## Wi-Fi and MQTT

With Wi-Fi connected and an MQTT broker configured, firmware publishes retained Home Assistant MQTT discovery for a firmware Update entity. It checks `releases/latest/download/update.json` on MQTT connection and every six hours. Home Assistant displays an available update; pressing Install sends a non-retained `INSTALL` to `<gateway-topic>/update/install`. The device downloads the application image over HTTPS and publishes progress to `<gateway-topic>/update/state`, then restarts. It does not install merely because a newer release exists.

Home Assistant's MQTT integration must use the same broker. Discovery uses the standard `homeassistant` prefix. Retained installation commands and fragmented/invalid commands are ignored. Concurrent Wi-Fi/Zigbee installs are rejected. A failed download leaves the current boot partition selected and resets update progress; users can retry.

## ZHA

Install the updated ELECRAM quirk and OTA-capable firmware first. Older quirk variants remain available; the new variant preserves standard output cluster `0x0019` on endpoint 10. Existing paired devices may need a fresh interview or re-pairing to expose the added cluster.

Until an ELECRAM provider is accepted upstream, add this to Home Assistant's configuration and restart:

```yaml
zha:
  zigpy_config:
    ota:
      extra_providers:
        - type: zigpy_remote
          url: https://github.com/IMMRMKW/ramses_esp/releases/latest/download/zha-ota.json
```

ZHA checks the catalogue and supplies firmware through its OTA server. Users install through the standard device update entity. The ESP discovers the coordinator's OTA endpoint and queries daily. No firmware traffic is sent through the RAMSES text clusters.

## Zigbee2MQTT

Use the ELECRAM definition from the converter development branch, which registers legacy and native text commands and enables OTA support. Until an OTA catalogue contribution is accepted upstream, add:

```yaml
ota:
  zigbee_ota_override_index_location: https://github.com/IMMRMKW/ramses_esp/releases/latest/download/z2m-ota.json
```

Enable Zigbee2MQTT's Home Assistant integration for its update entity. Keep the RAMSES external extension installed for communication with ramses_cc. This extension now handles identified native chunks, serializes complete outgoing messages per device, and aborts failed transfers without replaying a partial message. The converter enables native mode only when the firmware advertises OTA; older firmware retains its legacy commands.

## Release files

The tag workflow publishes:

- `ramses_esp_esp32_c6.bin`: merged image for USB flashing at address 0.
- `ramses_esp_c6.bin`: application-only image for HTTPS OTA.
- `ramses_esp_c6.ota`: standard Zigbee container holding the same application.
- `update.json`, `zha-ota.json`, `z2m-ota.json`: compatible catalogues and checksums.

Release tags must be `vMAJOR.MINOR` or `vMAJOR.MINOR.PATCH`, with components <=255. The tag is embedded in the application and packed into the Zigbee file version as `major << 24 | minor << 16 | patch << 8`. CI rejects wrong project/version/chip images, merged images, and images exceeding the OTA slot.

The initial implementation uses experimental manufacturer code `0xffff`, image type `0xc601`, and hardware revision 1. These must remain consistent in firmware and catalogues; obtain an assigned manufacturer code before broad upstream distribution. The catalogues also scope matching to ELECRAM/Ramses_esp32c6. New hardware revisions require distinct compatibility ranges or image types.

Zigbee verifies sub-element framing, image size, project identity, newer file version, and ESP image integrity before selecting the new boot partition. Its inactive-partition writer expires after five minutes without transfer activity. Downloads can take considerable time on Zigbee networks.

Existing devices need one initial manual upgrade to gain these features. Merely publishing these files cannot add an OTA client to old Zigbee firmware.

## Validation before release

Test a real older-to-newer update over Wi-Fi, ZHA, and Zigbee2MQTT. Verify that an interrupted download, power loss before switching partitions, a corrupt image, a repeated Install command, and a device restart all leave a recoverable device. Verify RAMSES traffic resumes after updating and that pairing/network settings survive. Software checks alone do not validate radio delivery or hardware reboot behaviour.

References: [MQTT Update](https://www.home-assistant.io/integrations/update.mqtt/), [ZHA OTA](https://www.home-assistant.io/integrations/zha/#ota-updates-of-zigbee-device-firmware), [Z2M OTA](https://www.zigbee2mqtt.io/guide/usage/ota_updates.html), [Espressif's release-v1 OTA client](https://github.com/espressif/esp-zigbee-sdk/blob/release/v1.0/examples/esp_zigbee_ota/ota_client/main/esp_ota_client.c).
