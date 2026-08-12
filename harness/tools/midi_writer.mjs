/**
 * Minimal format-0 SMF writer, shared by the probe generators.
 */

export const TPQ = 480;

function varLen(value) {
    const bytes = [value & 0x7f];
    let v = value >> 7;
    while (v > 0) {
        bytes.unshift((v & 0x7f) | 0x80);
        v >>= 7;
    }
    return bytes;
}

/** events: [{ tick, data: number[] }]; emitted in tick order, stably. */
export function buildFile(events) {
    const sorted = events
        .map((e, i) => ({ ...e, i }))
        .sort((a, b) => a.tick - b.tick || a.i - b.i);

    const body = [];
    let last = 0;
    for (const e of sorted) {
        body.push(...varLen(e.tick - last), ...e.data);
        last = e.tick;
    }
    body.push(...varLen(0), 0xff, 0x2f, 0x00); // end of track

    const be32 = (v) => [
        (v >> 24) & 0xff,
        (v >> 16) & 0xff,
        (v >> 8) & 0xff,
        v & 0xff
    ];

    return Buffer.from([
        0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, // MThd
        0, 0, // format 0
        0, 1, // one track
        (TPQ >> 8) & 0xff, TPQ & 0xff,
        0x4d, 0x54, 0x72, 0x6b, // MTrk
        ...be32(body.length),
        ...body
    ]);
}

export const tempo = (bpm, tick = 0) => {
    const us = Math.round(60_000_000 / bpm);
    return {
        tick,
        data: [0xff, 0x51, 0x03, (us >> 16) & 0xff, (us >> 8) & 0xff, us & 0xff]
    };
};

export const noteOn = (tick, ch, note, vel) => ({
    tick,
    data: [0x90 | ch, note, vel]
});
export const noteOff = (tick, ch, note) => ({ tick, data: [0x80 | ch, note, 0] });
export const program = (tick, ch, prog) => ({ tick, data: [0xc0 | ch, prog] });
export const cc = (tick, ch, controller, value) => ({
    tick,
    data: [0xb0 | ch, controller, value]
});
export const pitchBend = (tick, ch, value14) => ({
    tick,
    data: [0xe0 | ch, value14 & 0x7f, (value14 >> 7) & 0x7f]
});

/**
 * Roland GS parameter write, with the SC-8850 checksum.
 *
 * A SysEx event in a standard MIDI file is F0 followed by a variable-length
 * byte count and then the body including its terminating F7 — not the raw
 * bytes.  Emitting it raw corrupts every event after it in the track.
 */
export const gs = (tick, a1, a2, a3, data) => {
    const sum = a1 + a2 + a3 + data.reduce((s, c) => s + c, 0);
    const checksum = (128 - (sum % 128)) & 0x7f;
    const body = [0x41, 0x10, 0x42, 0x12, a1, a2, a3, ...data, checksum, 0xf7];
    return { tick, data: [0xf0, ...varLen(body.length), ...body] };
};
