#!/usr/bin/env node
/**
 * Generates a handful of deliberately trivial MIDI files.
 *
 * The upstream generated corpus exercises SysEx, effects and controllers all
 * at once, which makes it a poor first place to look when the two engines
 * disagree.  These probes isolate one mechanism each, so a divergence can be
 * attributed instead of merely observed.
 *
 *   node harness/tools/make_probes.mjs [outDir]
 */

import * as fs from "node:fs/promises";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

const TPQ = 480;

function varLen(value) {
    const bytes = [value & 0x7f];
    let v = value >> 7;
    while (v > 0) {
        bytes.unshift((v & 0x7f) | 0x80);
        v >>= 7;
    }
    return bytes;
}

/** events: [{ tick, data: number[] }], emitted in tick order. */
function buildTrack(events) {
    const sorted = [...events].sort((a, b) => a.tick - b.tick);
    const bytes = [];
    let last = 0;
    for (const e of sorted) {
        bytes.push(...varLen(e.tick - last), ...e.data);
        last = e.tick;
    }
    bytes.push(...varLen(0), 0xff, 0x2f, 0x00); // end of track

    const header = [
        0x4d, 0x54, 0x72, 0x6b,
        (bytes.length >> 24) & 0xff,
        (bytes.length >> 16) & 0xff,
        (bytes.length >> 8) & 0xff,
        bytes.length & 0xff
    ];
    return Buffer.from([...header, ...bytes]);
}

function buildFile(events) {
    const header = Buffer.from([
        0x4d, 0x54, 0x68, 0x64,
        0x00, 0x00, 0x00, 0x06,
        0x00, 0x00, // format 0
        0x00, 0x01, // one track
        (TPQ >> 8) & 0xff, TPQ & 0xff
    ]);
    return Buffer.concat([header, buildTrack(events)]);
}

function tempo(bpm) {
    const usPerQuarter = Math.round(60_000_000 / bpm);
    return {
        tick: 0,
        data: [
            0xff, 0x51, 0x03,
            (usPerQuarter >> 16) & 0xff,
            (usPerQuarter >> 8) & 0xff,
            usPerQuarter & 0xff
        ]
    };
}

const noteOn = (tick, ch, note, vel) => ({ tick, data: [0x90 | ch, note, vel] });
const noteOff = (tick, ch, note) => ({ tick, data: [0x80 | ch, note, 0] });
const program = (tick, ch, prog) => ({ tick, data: [0xc0 | ch, prog] });
const cc = (tick, ch, controller, value) => ({
    tick,
    data: [0xb0 | ch, controller, value]
});
const pitchBend = (tick, ch, value14) => ({
    tick,
    data: [0xe0 | ch, value14 & 0x7f, (value14 >> 7) & 0x7f]
});

const PROBES = {
    // One note, one instrument, nothing else. If this diverges, the
    // difference is in the oscillator/envelope path, not in control handling.
    "probe_single_note": [
        tempo(120),
        program(0, 0, 0),
        noteOn(0, 0, 60, 100),
        noteOff(TPQ * 2, 0, 60)
    ],

    // Same note held long enough for the release tail to dominate.
    "probe_single_note_release": [
        tempo(120),
        program(0, 0, 0),
        noteOn(0, 0, 60, 100),
        noteOff(TPQ, 0, 60)
    ],

    // Sweeps the keyboard: isolates pitch/interpolation differences, which
    // show up as a rising error with note number.
    "probe_chromatic": [
        tempo(120),
        program(0, 0, 0),
        ...Array.from({ length: 24 }, (_, i) => [
            noteOn(i * (TPQ / 2), 0, 48 + i * 2, 100),
            noteOff(i * (TPQ / 2) + TPQ / 4, 0, 48 + i * 2)
        ]).flat()
    ],

    // Velocity layers, one note per step.
    "probe_velocity": [
        tempo(120),
        program(0, 0, 0),
        ...Array.from({ length: 8 }, (_, i) => [
            noteOn(i * TPQ, 0, 60, 16 * (i + 1) - 1),
            noteOff(i * TPQ + TPQ / 2, 0, 60)
        ]).flat()
    ],

    // Continuous pitch bend across a held note: exercises the pitch update
    // path at whatever rate each engine recomputes it.
    "probe_pitch_bend": [
        tempo(120),
        program(0, 0, 0),
        noteOn(0, 0, 60, 100),
        ...Array.from({ length: 64 }, (_, i) =>
            pitchBend(i * (TPQ / 16), 0, Math.round(8192 + 4000 * Math.sin(i / 6)))
        ),
        noteOff(TPQ * 4, 0, 60)
    ],

    // Volume and pan sweeps on a sustained note.
    "probe_cc_volume_pan": [
        tempo(120),
        program(0, 0, 0),
        noteOn(0, 0, 60, 100),
        ...Array.from({ length: 32 }, (_, i) => [
            cc(i * (TPQ / 8), 0, 7, Math.round(127 - i * 3)),
            cc(i * (TPQ / 8), 0, 10, Math.round(i * 4))
        ]).flat(),
        noteOff(TPQ * 4, 0, 60)
    ],

    // Reverb and chorus sends on a sustained note: isolates the effect buses.
    "probe_reverb_chorus": [
        tempo(120),
        program(0, 0, 0),
        cc(0, 0, 91, 127),
        cc(0, 0, 93, 127),
        noteOn(0, 0, 60, 100),
        noteOff(TPQ, 0, 60)
    ],

    // Drum channel (channel 10) with a single hit.
    "probe_drum": [
        tempo(120),
        noteOn(0, 9, 36, 100),
        noteOff(TPQ / 2, 9, 36)
    ],

    // A dense chord: exercises voice mixing and any summation order effects.
    "probe_chord": [
        tempo(120),
        program(0, 0, 0),
        ...[60, 64, 67, 71, 74].map((n) => noteOn(0, 0, n, 100)),
        ...[60, 64, 67, 71, 74].map((n) => noteOff(TPQ * 2, 0, n))
    ]
};

const outDir = path.resolve(
    process.argv[2] ??
        path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "probes")
);
await fs.mkdir(outDir, { recursive: true });

for (const [name, events] of Object.entries(PROBES)) {
    const file = path.join(outDir, `${name}.mid`);
    await fs.writeFile(file, buildFile(events));
    console.log(`wrote ${file}`);
}
console.log(`${Object.keys(PROBES).length} probe files in ${outDir}`);
