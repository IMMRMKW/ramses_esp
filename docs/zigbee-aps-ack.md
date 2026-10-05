# Zigbee native delivery and message assembly

This development branch uses native APS delivery confirmations rather than
ASCII `ACK seq/total` traffic. A complete RF message is serialized into chunks;
the next chunk is submitted only after the current APS request confirms success.
ZCL default responses are disabled. APS confirmation means arrival at the peer's
Zigbee stack, not Home Assistant consumption or RF execution.

## Wire format and ZHA quirk

Endpoint 10 and clusters 0xFC00 (ESP to host) / 0xFC01 (host to ESP) stay the same.
The updated ELECRAM quirk defines these commands:

| Command | Purpose | Payload |
| --- | --- | --- |
| 0x00 | Legacy text | ZCL char-string, existing ASCII chunk header |
| 0x01 | Legacy ASCII ACK | ZCL char-string |
| 0x02 | Native identified chunk | uint32 message ID, uint8 sequence, uint8 total, char-string body |
| 0x03 | Select native mode (host to ESP) | Boolean |

Native chunks have at most 30 ASCII body bytes. The binary ID/sequence/total
fields take six bytes and the char-string length takes one. Including the
three-byte ZCL header, the ASDU is at most 40 bytes. The quirk explicitly
disables the manufacturer header for native commands, matching the firmware.
The ESP emits native command 2 with server-to-client direction (frame control
0x19). Host writes use client-to-server direction (0x11). The quirk also decodes
legacy command 0 with its existing client-to-server direction.
The quirk matches both the deployed device type 0x0000 and the current
firmware's simple-sensor descriptor (0x000C).

The host selects native mode at connection and reconnection with command 3.
Receiving a native data command also enables native mode on the firmware.
Until selected, the ESP retains its original command-0 text format, using APS
confirmation to pace transmission. Legacy command definitions are retained
so already deployed firmware and hosts can still use their text/ACK protocol.
Native mode lasts until reboot or command 3 disables it. A watchdog recovery
preserves the negotiated mode in RTC memory; a power cycle selects legacy
until the host negotiates or sends native data again.

## Missing messages and recovery

- Each complete native message receives its own 32-bit ID. Counters start from
  a random value at boot/transport creation to reduce reuse across restarts.
- Receivers require chunk 1 to start a message. Subsequent chunks must match
  its ID, total and expected next sequence. A mismatched ID is ignored; a gap
  or inconsistent total aborts the incomplete message. A new first chunk can
  replace an incomplete predecessor without combining their bytes.
- Accepted duplicate chunks are ignored. Repeated delivery of the most
  recently completed ID is ignored. This is not persistent exactly-once RF
  execution across reboots or arbitrary replay histories.
- Partial assembly expires after 35 seconds. Only a complete message reaches
  the RF parser or Home Assistant RAMSES parser.
- An APS delivery failure aborts the outgoing message. Neither endpoint
  blindly retries the whole RF command through alternate command APIs.
- The host waits at most 35 seconds for each native delivery; failure releases
  its write lock so another message can proceed with a different ID.
- If the ESP receives no APS confirmation for 30 seconds, it reboots to
  reclaim stack buffers. Opening the in-flight gate without releasing the
  stack's request would be unsafe. Recovery can lose queued messages.
- The ESP processes incoming commands while waiting for APS confirmation.

Yes, messages can be dropped after exhausted delivery retries, incomplete
assembly, oversized/malformed input, bounded queue overflow or restart. APS
cannot acknowledge application queue capacity. The old ASCII ACK was also
sent before full message validation/execution, so it was not an end-to-end
execution guarantee. RF echoes and RAMSES replies remain the end-to-end
signals. Traffic is not stored durably during outages.

## Installation and traffic

Install the coordinated firmware, `ramses_rf` transport changes and updated
ELECRAM quirk. Then configure through `ramses_cc`:

```text
zigbee://<ieee>/0xfc00/0x0000/10/0xfc01/0x0000/10?ack=aps
```

The stable dependency pin in `ramses_cc` still needs a published transport
version or accessible immutable commit before deployment. A missing updated
quirk produces a clear transport initialization error. APS mode is not for
old firmware that waits for ASCII ACKs. The device-selection UI keeps legacy
URLs because it cannot identify firmware capability.

Native IDs add overhead, offset by removing one ASCII ACK command per chunk.
For example, 96 ASCII characters need four 30-byte native chunks, versus three
32-byte legacy data commands plus three ASCII ACK commands. MAC/APS ACKs and
stack retries still use airtime; this is not a claim of halving total airtime.
Capture traffic during RF bursts and discovery to quantify the actual saving.

## Validation

The firmware builds with ESP-IDF 5.5.2, esp-zigbee-lib 1.6.8 and esp-zboss-lib
1.6.4. Host tests cover message isolation, missing/late/duplicate chunks,
timeouts, ordered concurrent writes and payload bounds. Quirk tests verify
the exact native wire bytes and legacy decoding.

Hardware testing remains required: verify actual APS confirms, remove the
coordinator during transfer, exercise queue overflow and the watchdog, and
capture that incomplete messages never become RF commands. Tests do not prove
over-the-air SDK behaviour or eliminate packet loss.

References:
- [ZHA review](https://github.com/zigpy/zha-device-handlers/pull/4782#issuecomment-4016569965)
- [Espressif 1.x APS API](https://github.com/espressif/esp-zigbee-sdk/blob/release/v1.0/components/esp-zigbee-lib/include/aps/esp_zigbee_aps.h)
- [Espressif APS example](https://github.com/espressif/esp-zigbee-sdk/blob/release/v1.0/components/esp-zigbee-console/src/cli_cmd_aps.c)
- [zigpy command and reply APIs](https://github.com/zigpy/zigpy/blob/dev/zigpy/zcl/__init__.py)
