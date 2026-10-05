# Zigbee delivery acknowledgements

The `feat/zigbee-aps-ack` firmware sends the existing command 0 on cluster
0xFC00, endpoint 10, with the same length-prefixed ASCII `seq/total|body`
payload. Bodies remain 32 bytes to avoid introducing APS fragmentation.
The ESP explicitly requests an APS ACK and waits for its APS data-confirm
callback before submitting another chunk. It disables ZCL default responses
and no longer sends or waits for ASCII `ACK seq/total` commands.

One request is in flight at a time. Pending state is armed before submission,
and confirmations are matched against the destination, endpoints and complete
ASDU. A delivery failure aborts the message; APS owns retransmission. A timeout
does not release the buffer gate until the actual confirmation arrives. If no
confirmation ever arrives, transmission stays blocked until reboot rather
than overflowing the stack's buffers. Incoming RF commands are processed while
the task waits. Chunk assembly also rejects inconsistent totals and expires
incomplete messages after 35 seconds.

## Host compatibility

Use the corresponding `ramses_rf` transport branch through `ramses_cc`, and
append `?ack=aps` to the bridge URL:

```text
zigbee://<ieee>/0xfc00/0x0000/10/0xfc01/0x0000/10?ack=aps
```

The host requests APS ACKs (`ask_for_ack=True`), disables ZCL default
responses and does not generate ASCII ACKs. Complete outgoing frames are
serialized so concurrent requests cannot interleave chunks. A failed chunk
stops the frame instead of sending the remaining pieces or probing alternate
command APIs after an uncertain delivery.

URLs without `ack=aps` keep the host's legacy behaviour for older firmware.
The new firmware ignores legacy ACK text, so an older host can still receive
its messages, but it will continue producing unnecessary control traffic.
Do not select APS mode with old firmware: that firmware still waits for ASCII
ACKs and will time out. The existing ZHA quirk's command definitions remain
usable; removing its unused ACK commands can be a later coordinated change.

## Traffic and delivery guarantees

For an N-chunk message, the old protocol adds N ASCII ACK commands. APS mode
removes those commands; it still uses MAC/APS acknowledgements and stack
retries. For example, a 96-character message has three 32-character chunks:
three data commands instead of three data plus three ASCII ACK commands.
This halves custom application commands, not total airtime.

The reviewer's question about bandwidth is reasonable, but the PR's stated
rate of one message per 1–2 minutes is not evidence of network saturation.
Capture real traffic during discovery, RF bursts and retries before making a
network-capacity claim. ASCII remains useful because the bridge transports an
existing RAMSES serial protocol rather than exposing each RF device as a
separate Zigbee device. Binary encoding or larger chunks are separate changes.

An APS ACK confirms delivery to the remote Zigbee stack. It does not confirm
Home Assistant consumption, queue acceptance, RF transmission or a response
from the heating/ventilation device. RF echoes and RAMSES replies remain the
end-to-end signals. The RX queue is bounded; APS cannot prevent application
queue overflow. An explicit ZCL default response would be needed to convey
application acceptance/errors if that guarantee is required.

## Validation before deployment

- Build on ESP-IDF 5.5.2 with esp-zigbee-lib 1.6.8 and esp-zboss-lib 1.6.4.
- Run the transport tests, including real zigpy command serialization,
  delivery pacing, failed-delivery abortion and concurrent-frame ordering.
- On hardware, capture both directions and verify APS ACK requests, absence
  of ASCII ACK commands and default responses, RF echoes and multi-chunk data.
- Test a coordinator outage, a dropped chunk and RF bursts. Confirm that
  unrelated APS confirmations cannot advance the next chunk and that the
  stack resumes safely after a late confirmation.
- Measure queue drops and packet/airtime counts on the intended Zigbee network.

The automated build and host tests do not validate over-the-air SDK callback
behaviour. This branch needs hardware testing before release.

References:
- [ZHA review](https://github.com/zigpy/zha-device-handlers/pull/4782#issuecomment-4016569965)
- [Espressif 1.x APS API](https://github.com/espressif/esp-zigbee-sdk/blob/release/v1.0/components/esp-zigbee-lib/include/aps/esp_zigbee_aps.h)
- [Espressif APS example](https://github.com/espressif/esp-zigbee-sdk/blob/release/v1.0/components/esp-zigbee-console/src/cli_cmd_aps.c)
- [zigpy command and reply APIs](https://github.com/zigpy/zigpy/blob/dev/zigpy/zcl/__init__.py)
