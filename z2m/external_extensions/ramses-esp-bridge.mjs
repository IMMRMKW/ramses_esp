/**
 * Zigbee2MQTT External Extension: RAMSES ESP32-C6 Bridge
 *
 * Bridges ELECRAM Ramses_esp32c6 Zigbee clusters ↔ MQTT topics so that
 * ramses_cc (via mqtt_bridge.py) can communicate with the ESP32 device
 * through zigbee2mqtt.
 *
 * Data flow:
 *   ESP32 ──(ZCL cmd 0xFC00)──► Z2M extension ──► RAMSES/GATEWAY/{ieee}/rx
 *                                                   payload: {"msg": "FRAME\r\n"}
 *
 *   RAMSES/GATEWAY/{ieee}/tx ──► Z2M extension ──(ZCL cmd setText 0xFC01)──► ESP32
 *   payload: {"msg": "FRAME\r\n"}
 *
 *   RAMSES/GATEWAY/{ieee}/cmd/cmd  (!V etc.) ──► Z2M extension generates response
 *   RAMSES/GATEWAY/{ieee}/cmd/result           ◄── {"cmd":"!V","return":"# evofw3 ..."}
 *
 * Prerequisites:
 *   - elecram.ts device definition installed in zigbee-herdsman-converters
 *     (registers ramsesRxCluster / ramsesTxCluster custom clusters)
 *
 * Install:
 *   Copy this file to <z2m-data>/external_extensions/ramses-esp-bridge.mjs
 *   Restart zigbee2mqtt.
 *
 * Configuration:
 *   Edit the constants below to match your ramses_cc setup.
 */

// ── Configurable constants ─────────────────────────────────────────────────────
/** Must match the topic_prefix configured in ramses_cc (mqtt_bridge.py). */
const RAMSES_TOPIC_PREFIX = "RAMSES/GATEWAY";

/** Zigbee endpoint on the ESP32 (HA_ESP_SENSOR_ENDPOINT in firmware). */
const RAMSES_ENDPOINT_ID = 10;

/** Custom cluster names as registered in elecram.ts. */
const RAMSES_RX_CLUSTER = "ramsesRxCluster"; // 0xFC00, ESP32 → coordinator
const RAMSES_TX_CLUSTER = "ramsesTxCluster"; // 0xFC01, coordinator → ESP32

/** Maximum usable body length per chunk (matches firmware buffer). */
const MAX_CHUNK_BODY = 32;

/** Maximum total ZCL CHAR_STRING length (length-byte limit in firmware). */
const MAX_CHUNK_TOTAL = 63;

/** ESP32 manufacturer / model strings (from Basic cluster). */
const RAMSES_MANUFACTURER = "ELECRAM";
const RAMSES_MODEL = "Ramses_esp32c6";

/** Delay (ms) between consecutive ZCL writes to avoid ZBOSS buffer exhaustion. */
const INTER_CHUNK_DELAY_MS = 25;
// ──────────────────────────────────────────────────────────────────────────────

export default class RamsesEspBridge {
    /**
     * @param {import('zigbee2mqtt/lib/zigbee').default} zigbee
     * @param {import('zigbee2mqtt/lib/mqtt').default}   mqtt
     * @param {*}                                        state
     * @param {Function}                                 publishEntityState
     * @param {import('zigbee2mqtt/lib/eventBus').default} eventBus
     * @param {Function}                                 enableDisableExtension
     * @param {Function}                                 restartCallback
     * @param {Function}                                 addExtension
     * @param {*}                                        settings
     * @param {import('zigbee2mqtt/lib/util/logger').default} logger
     */
    constructor(zigbee, mqtt, state, publishEntityState, eventBus, enableDisableExtension, restartCallback, addExtension, settings, logger) {
        this.zigbee = zigbee;
        this.mqtt = mqtt;
        this.eventBus = eventBus;
        this.logger = logger;

        /**
         * Per-device chunk reassembly buffers.
         * Key: IEEE address string.
         * Value: { total: number, parts: (string|null)[], received: number }
         * @type {Map<string, {total: number, parts: (string|null)[], received: number}>}
         */
        this.chunkBuffers = new Map();
    }

    async start() {
        // Subscribe to all RAMSES topics so ramses_cc TX/CMD messages are visible.
        await this.mqtt.subscribe(`${RAMSES_TOPIC_PREFIX}/#`);

        // Zigbee → MQTT: called for every incoming ZCL message from every device.
        this.eventBus.onDeviceMessage(this, (data) => this._onDeviceMessage(data));

        // MQTT → Zigbee: called for every inbound MQTT message on any subscribed topic.
        this.eventBus.onMQTTMessage(this, (data) => this._onMqttMessage(data));

        this.logger.info(`[RamsesEspBridge] Started — listening on ${RAMSES_TOPIC_PREFIX}/#`);
    }

    stop() {
        this.eventBus.removeListeners(this);
        this.logger.info("[RamsesEspBridge] Stopped");
    }

    // ── Zigbee → MQTT (RX path) ───────────────────────────────────────────────

    /**
     * Handle an incoming ZCL message from a Zigbee device.
     * Filters for ELECRAM Ramses_esp32c6 on cluster ramsesRxCluster (0xFC00),
     * reassembles any chunked payloads, then publishes to
     * RAMSES/GATEWAY/{ieee}/rx as {"msg": "FRAME\r\n"}.
     */
    _onDeviceMessage(data) {
        const device = data.device;
        if (!device) return;

        // Filter: ELECRAM Ramses_esp32c6 only.
        if (device.zh.manufacturerName !== RAMSES_MANUFACTURER) return;
        if (device.zh.modelID !== RAMSES_MODEL) return;

        // Filter: cluster ramsesRxCluster (0xFC00).
        if (data.cluster !== RAMSES_RX_CLUSTER) return;

        // Extract text from the 'sendText' command payload ({text: "..."}).
        const text = this._extractText(data.data);
        if (!text) {
            this.logger.debug("[RamsesEspBridge] RX: no decodable text payload, skipping");
            return;
        }

        // Application-level ACKs from the ESP firmware are control-plane only.
        if (text.startsWith("ACK ")) return;

        const ieee = device.zh.ieeeAddr;

        if (this._isChunk(text)) {
            const assembled = this._handleChunk(ieee, text);
            if (assembled !== null) {
                this._publishRx(ieee, assembled);
            }
        } else {
            this._publishRx(ieee, text);
        }
    }

    // ── MQTT → Zigbee (TX / CMD paths) ───────────────────────────────────────

    /**
     * Handle an inbound MQTT message.
     * Dispatches to TX handler (radio frames) or CMD handler (commands like !V).
     */
    _onMqttMessage(data) {
        const {topic, message} = data;

        // Ignore topics that are not under our RAMSES prefix.
        if (!topic.startsWith(RAMSES_TOPIC_PREFIX + "/")) return;

        // rest = e.g. "0x00124b0018f5a394/tx" or "0x00124b0018f5a394/cmd/cmd"
        const rest = topic.slice(RAMSES_TOPIC_PREFIX.length + 1);
        const slashIdx = rest.indexOf("/");
        if (slashIdx === -1) return;

        const ieee = rest.slice(0, slashIdx);
        const suffix = rest.slice(slashIdx + 1); // "tx" | "cmd/cmd" | ...

        if (suffix === "tx") {
            this._handleTxMessage(ieee, message);
        } else if (suffix === "cmd/cmd") {
            this._handleCmdMessage(ieee, message);
        }
    }

    /**
     * Forward a RAMSES radio frame from ramses_cc to the ESP32 via ZCL attribute write.
     * Payload format: {"msg": "FRAME\r\n"}
     */
    _handleTxMessage(ieee, message) {
        let frame;
        try {
            const json = JSON.parse(message);
            if (typeof json.msg !== "string") return;
            frame = json.msg.replace(/[\r\n]+$/, ""); // strip trailing CRLF
        } catch {
            // Not JSON — treat raw string as the frame.
            frame = message.trim();
        }
        if (!frame) return;

        const endpoint = this._getEndpoint(ieee);
        if (!endpoint) {
            this.logger.warn(`[RamsesEspBridge] TX: no endpoint for ieee=${ieee}`);
            return;
        }

        const chunks = this._chunkPayload(frame);
        this._sendChunksAsync(endpoint, chunks, ieee).catch((err) => {
            this.logger.error(`[RamsesEspBridge] TX send error: ${err.message}`);
        });
    }

    /**
     * Handle a ramses_cc command (e.g. !V).
     * The ESP32 firmware has no mechanism to respond to arbitrary commands,
     * so the extension generates synthetic replies for known commands.
     */
    _handleCmdMessage(ieee, message) {
        const cmd = message.trim();
        this.logger.debug(`[RamsesEspBridge] CMD "${cmd}" for ieee=${ieee}`);

        if (cmd === "!V" || cmd.startsWith("!V ")) {
            // ramses_cc requires an evofw3-compatible version string to pass its
            // firmware handshake FSM (see mqtt_bridge.py _handle_cmd_message).
            const payload = JSON.stringify({cmd: "!V", return: "# evofw3 ramses_esp32c6 1.0.0"});
            this._publishResult(ieee, payload);
        }
        // Other commands (e.g. !C) would require firmware support and are not forwarded.
    }

    // ── MQTT publish helpers ──────────────────────────────────────────────────

    /** Publish assembled frame: RAMSES/GATEWAY/{ieee}/rx  {"msg": "FRAME\r\n"} */
    _publishRx(ieee, frame) {
        const payload = JSON.stringify({msg: `${frame}\r\n`});
        this.mqtt.publish(`${ieee}/rx`, payload, {baseTopic: RAMSES_TOPIC_PREFIX, skipReceive: false});
        this.logger.debug(`[RamsesEspBridge] RX ieee=${ieee}: ${frame}`);
    }

    /** Publish command result: RAMSES/GATEWAY/{ieee}/cmd/result */
    _publishResult(ieee, payload) {
        this.mqtt.publish(`${ieee}/cmd/result`, payload, {baseTopic: RAMSES_TOPIC_PREFIX, skipReceive: false});
        this.logger.debug(`[RamsesEspBridge] CMD result ieee=${ieee}: ${payload}`);
    }

    // ── ZCL send helpers ──────────────────────────────────────────────────────

    /**
     * Resolve the zigbee-herdsman Endpoint for the given IEEE address.
     * @returns {import('zigbee-herdsman/dist/controller/model/endpoint').default | null}
     */
    _getEndpoint(ieee) {
        // resolveEntity returns the Z2M Device wrapper; .zh is the zh Device.
        const entity = this.zigbee.resolveEntity(ieee);
        if (entity && entity.zh) {
            const ep = entity.zh.getEndpoint(RAMSES_ENDPOINT_ID);
            if (ep) return ep;
        }
        // Fallback: iterate all known devices.
        for (const device of this.zigbee.devicesIterator()) {
            if (device.zh.ieeeAddr === ieee) {
                const ep = device.zh.getEndpoint(RAMSES_ENDPOINT_ID);
                if (ep) return ep;
            }
        }
        return null;
    }

    /**
     * Sequentially send chunks to the ESP32 via ZCL cluster-specific command on
     * cluster ramsesTxCluster (0xFC01), command 'setText' (id=0x00).
     * Mirrors the command mode used by zigbee.py and the ZHA quirk.
     *
     * @param {*} endpoint
     * @param {[number, number, string][]} chunks  [[seq, total, chunkStr], ...]
     * @param {string} ieee
     */
    async _sendChunksAsync(endpoint, chunks, ieee) {
        for (const [seq, total, chunk] of chunks) {
            try {
                await endpoint.command(RAMSES_TX_CLUSTER, "setText", {text: chunk}, {disableDefaultResponse: true});
                this.logger.debug(`[RamsesEspBridge] TX chunk ${seq}/${total} → ieee=${ieee}: ${chunk}`);
            } catch (err) {
                this.logger.error(`[RamsesEspBridge] TX chunk ${seq}/${total} command failed (ieee=${ieee}): ${err.message}`);
            }
            if (seq < total) {
                await this._sleep(INTER_CHUNK_DELAY_MS);
            }
        }
    }

    // ── Chunking helpers (mirrors zigbee.py _chunk_payload logic) ─────────────

    /** Return true if text matches the chunk header format "seq/total|..." */
    _isChunk(text) {
        return /^\d{1,3}\/\d{1,3}\|/.test(text);
    }

    /**
     * Buffer a single received chunk and return the assembled string once all
     * parts have arrived, or null if assembly is still in progress.
     * @param {string} ieee  Per-device reassembly key.
     * @param {string} text  Raw chunk string "seq/total|body".
     * @returns {string | null}
     */
    _handleChunk(ieee, text) {
        const match = text.match(/^(\d{1,3})\/(\d{1,3})\|(.*)$/s);
        if (!match) return text; // malformed — pass through as-is

        const seq = parseInt(match[1], 10);
        const total = parseInt(match[2], 10);
        const body = match[3];

        if (seq < 1 || total < 1 || seq > total) return text; // invalid — pass through

        let buf = this.chunkBuffers.get(ieee);
        if (!buf || buf.total !== total) {
            buf = {total, parts: new Array(total).fill(null), received: 0};
            this.chunkBuffers.set(ieee, buf);
        }

        if (buf.parts[seq - 1] === null) {
            buf.parts[seq - 1] = body;
            buf.received += 1;
        }

        if (buf.received < total) return null; // more chunks expected

        const assembled = buf.parts.map((p) => (p === null ? "" : p)).join("");
        this.chunkBuffers.delete(ieee);
        return assembled;
    }

    /**
     * Split a frame into [[seq, total, chunkString], ...] tuples.
     * Mirrors zigbee.py ZigbeeTransport._chunk_payload.
     * @param {string} frame
     * @returns {[number, number, string][]}
     */
    _chunkPayload(frame) {
        if (frame.length <= MAX_CHUNK_TOTAL) {
            return [[1, 1, frame]];
        }
        const total = Math.ceil(frame.length / MAX_CHUNK_BODY);
        const chunks = [];
        for (let i = 0; i < total; i++) {
            const body = frame.slice(i * MAX_CHUNK_BODY, (i + 1) * MAX_CHUNK_BODY);
            const header = `${i + 1}/${total}|`;
            const allowed = MAX_CHUNK_TOTAL - header.length;
            chunks.push([i + 1, total, header + body.slice(0, allowed)]);
        }
        return chunks;
    }

    // ── Misc helpers ──────────────────────────────────────────────────────────

    /**
     * Extract the text string from a 'sendText' command's data payload.
     * Handles the decoded {text: string} form and raw byte-array fallback.
     * @param {*} data  data.data from a DeviceMessage
     * @returns {string | null}
     */
    _extractText(data) {
        if (!data) return null;

        // Normal case: zigbee-herdsman decoded the CHAR_STR parameter as {text: "..."}.
        if (typeof data.text === "string") return data.text || null;

        // Fallback: raw byte array in ZCL CHAR_STRING wire format [length, ...chars].
        if (Array.isArray(data) && data.length >= 2) {
            const len = data[0];
            if (len > 0 && len < data.length) {
                return Buffer.from(data.slice(1, 1 + len)).toString("ascii") || null;
            }
        }

        return null;
    }

    /** @param {number} ms */
    _sleep(ms) {
        return new Promise((resolve) => setTimeout(resolve, ms));
    }
}
