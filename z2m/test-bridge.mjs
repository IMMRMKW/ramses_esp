import assert from "node:assert/strict";
import {test} from "node:test";
import Bridge from "./external_extensions/ramses-esp-bridge.mjs";

function bridge() {
    return new Bridge(null, null, null, null, null, null, null, null, null, {
        debug() {}, error() {}, warn() {}, info() {},
    });
}

test("missing and late chunks never combine messages", () => {
    const b = bridge();
    const chunk = (id, sequence, total, text) => b._handleNativeChunk("a", {message_id: id, sequence, total, text});
    assert.equal(chunk(1, 1, 3, "first"), null);
    assert.equal(chunk(1, 3, 3, "missing"), null);
    assert.equal(chunk(2, 1, 2, "next"), null);
    assert.equal(chunk(1, 2, 3, "late"), null);
    assert.equal(chunk(2, 2, 2, " frame"), "next frame");
    assert.equal(chunk(2, 1, 2, "next"), null);
});

test("native writes abort on delivery failure and next frame uses a fresh ID", async () => {
    const b = bridge();
    const calls = [];
    let fail = true;
    const endpoint = {async command(cluster, command, payload) {
        calls.push(payload);
        if (fail && payload.sequence === 2) throw new Error("delivery failed");
    }};
    await assert.rejects(b._sendNative(endpoint, "x".repeat(80)), /delivery failed/);
    assert.equal(calls.length, 2);
    fail = false;
    await b._sendNative(endpoint, "new");
    assert.notEqual(calls[0].message_id, calls[2].message_id);
    assert.equal(calls[2].sequence, 1);
    assert.equal(calls[2].text, "new");
});

test("complete frames are serialized per device", async () => {
    const b = bridge();
    const calls = [];
    b._getEndpoint = () => ({supportsOutputCluster: () => true});
    b._sendNative = async (_, frame) => {
        calls.push(`start:${frame}`);
        await new Promise(resolve => setTimeout(resolve, 5));
        calls.push(`end:${frame}`);
    };
    b._handleTxMessage("a", JSON.stringify({msg: "first"}));
    b._handleTxMessage("a", JSON.stringify({msg: "second"}));
    await b.pendingWrites.get("a");
    assert.deepEqual(calls, ["start:first", "end:first", "start:second", "end:second"]);
});
