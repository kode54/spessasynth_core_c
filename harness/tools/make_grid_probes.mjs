#!/usr/bin/env node
/**
 * Generates a single-note grid corpus.
 *
 * Modelled on the probe method in ../TabulaSonora (specv2/docs/COMPARING_RENDERS.md):
 * one note, one program, one parameter varied per axis, so a result is read
 * *down* a column rather than from any single file. A deficit that is constant
 * across an axis exonerates that path; one that grows with the parameter
 * indicts it; and a two-axis grid separates an interaction from either axis
 * alone — which is how that project caught a send feed that was not
 * pan-independent.
 *
 * Everything here is deliberately trivial: two seconds, one note, no effects
 * beyond the one under test. When a file from this corpus differs, the cause
 * has almost nowhere to hide.
 *
 *   node harness/tools/make_grid_probes.mjs [outDir]
 */

import * as fs from "node:fs/promises";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import {
    TPQ,
    buildFile,
    cc,
    channelPressure,
    gs,
    noteOff,
    noteOn,
    pitchBend,
    program,
    tempo
} from "./midi_writer.mjs";

const CH = 0;
const NOTE = 60;
const VEL = 100;
const PROGRAM = 0;
const HOLD = TPQ * 2; // two beats at 120 BPM = 1 s
const TAIL = TPQ * 2;

const CC_VOLUME = 7;
const CC_PAN = 10;
const CC_EXPRESSION = 11;
const CC_REVERB = 91;
const CC_CHORUS = 93;
const CC_DELAY = 94;

/**
 * One cell: a program change, some controller setup, one note, silence.
 * setup is a list of [controller, value] pairs applied before the note.
 */
function cell({
    setup = [],
    prog = PROGRAM,
    note = NOTE,
    vel = VEL,
    sysex = [],
    pre = [],
    noteCount = 1
}) {
    const events = [tempo(120), program(0, CH, prog)];
    for (const [controller, value] of setup) {
        events.push(cc(0, CH, controller, value));
    }
    for (const sx of sysex) {
        events.push(sx);
    }
    for (const e of pre) {
        events.push(e);
    }
    /* Successive notes are short and spaced so each is separable in the
     * render; a single-note cell keeps the original full-length hold. */
    const step = noteCount > 1 ? HOLD / 2 : HOLD;
    let at = TPQ / 4;
    for (let i = 0; i < noteCount; i++) {
        events.push(noteOn(at, CH, note, vel));
        events.push(noteOff(at + step - TPQ / 8, CH, note));
        at += step;
    }
    events.push(cc(at + TAIL, CH, CC_EXPRESSION, 127)); // pad the tail
    return buildFile(events);
}

const files = new Map();

/* ── Axis: pan, dry only ──────────────────────────────────────────────────
 * The dry path's deficit should be flat across this axis.  A pan law that
 * loses energy off centre shows up here and nowhere else. */
const PANS = [0, 32, 64, 94, 127];
for (const pan of PANS) {
    files.set(`grid_pan-${String(pan).padStart(3, "0")}_send-none`, cell({
        setup: [[CC_PAN, pan], [CC_REVERB, 0], [CC_CHORUS, 0], [CC_DELAY, 0]]
    }));
}

/* ── Axis: each send on its own, centred ─────────────────────────────────
 * A send whose deficit never rises above the dry baseline is right; one that
 * grows with the send level is not. */
const SENDS = [0, 40, 127];
for (const [name, controller] of [
    ["reverb", CC_REVERB],
    ["chorus", CC_CHORUS],
    ["delay", CC_DELAY]
]) {
    for (const level of SENDS) {
        files.set(`grid_${name}-${String(level).padStart(3, "0")}_pan-064`, cell({
            setup: [
                [CC_PAN, 64],
                [CC_REVERB, 0],
                [CC_CHORUS, 0],
                [CC_DELAY, 0],
                [controller, level]
            ]
        }));
    }
}

/* ── Grid: pan x chorus send ─────────────────────────────────────────────
 * Separates an interaction from either axis alone.  A send fed from a
 * pre-pan mono signal must show the same wet return at every pan; if the
 * error is a monotonic function of pan, the feed or the return is panned. */
for (const pan of PANS) {
    for (const level of [0, 127]) {
        files.set(
            `grid2_pan-${String(pan).padStart(3, "0")}_chorus-${String(level).padStart(3, "0")}`,
            cell({
                setup: [
                    [CC_PAN, pan],
                    [CC_REVERB, 0],
                    [CC_DELAY, 0],
                    [CC_CHORUS, level]
                ]
            })
        );
    }
}

/* ── Axis: velocity ──────────────────────────────────────────────────────
 * Velocity zone selection and the velocity-to-attenuation curve. */
for (const vel of [1, 16, 32, 64, 96, 127]) {
    files.set(`grid_velocity-${String(vel).padStart(3, "0")}`, cell({
        setup: [[CC_PAN, 64], [CC_REVERB, 0], [CC_CHORUS, 0], [CC_DELAY, 0]],
        vel
    }));
}

/* ── Axis: note, one per octave ──────────────────────────────────────────
 * Sample zone selection and the pitch/interpolation path. */
for (const note of [24, 36, 48, 60, 72, 84, 96]) {
    files.set(`grid_note-${String(note).padStart(3, "0")}`, cell({
        setup: [[CC_PAN, 64], [CC_REVERB, 0], [CC_CHORUS, 0], [CC_DELAY, 0]],
        note
    }));
}

/* ── Axis: channel volume and expression ─────────────────────────────────
 * Both scale the same voice; upstream applies them at different points. */
for (const level of [0, 32, 64, 100, 127]) {
    files.set(`grid_volume-${String(level).padStart(3, "0")}`, cell({
        setup: [[CC_PAN, 64], [CC_REVERB, 0], [CC_CHORUS, 0], [CC_DELAY, 0],
                [CC_VOLUME, level]]
    }));
    files.set(`grid_expression-${String(level).padStart(3, "0")}`, cell({
        setup: [[CC_PAN, 64], [CC_REVERB, 0], [CC_CHORUS, 0], [CC_DELAY, 0],
                [CC_EXPRESSION, level]]
    }));
}

/* ── Axis: GS velocity sense depth ───────────────────────────────────────
 * Address 40 1x 1a, part 1.  The depth scales incoming velocity by
 * depth/64, so 64 is unity and 100 pushes most velocities into the clamp. */
for (const depth of [0, 32, 64, 100, 127]) {
    files.set(`grid_vsensedepth-${String(depth).padStart(3, "0")}`, cell({
        setup: [[CC_PAN, 64], [CC_REVERB, 0], [CC_CHORUS, 0], [CC_DELAY, 0]],
        sysex: [gs(0, 0x40, 0x11, 0x1a, [depth])]
    }));
}

/* ── Axis: soft pedal ────────────────────────────────────────────────────
 * CC#67 drives a switch-curve modulator, so everything at or below 63
 * should be identical and everything from 64 up should be identical — the
 * interesting thing is the step, and that there is only one of them.
 * Upstream darkens the tone and does not also attenuate it. */
for (const level of [0, 32, 63, 64, 96, 127]) {
    files.set(`grid_softpedal-${String(level).padStart(3, "0")}`, cell({
        setup: [[CC_PAN, 64], [CC_REVERB, 0], [CC_CHORUS, 0], [CC_DELAY, 0],
                [67, level]]
    }));
}

/* ── Axes: one per default modulator source ─────────────────────────────
 * Each of these drives exactly one default modulator, so a column that
 * moves indicts that modulator's amount, curve or destination and nothing
 * else.  Sources that are not controllers get their own event. */
const MOD_LEVELS = [0, 32, 64, 96, 127];

for (const [name, controller] of [
    ["modwheel", 1],       // -> vibLfoToPitch, 50
    ["attack", 73],        // -> attackVolEnv, 6000
    ["release", 72],       // -> releaseVolEnv, 3600
    ["decay", 75],         // -> decayVolEnv, 3600
    ["brightness", 74],    // -> initialFilterFc, 9600
    ["resonance", 71]      // -> initialFilterQ, 250
]) {
    for (const level of MOD_LEVELS) {
        files.set(`grid_${name}-${String(level).padStart(3, "0")}`, cell({
            setup: [[CC_PAN, 64], [CC_REVERB, 0], [CC_CHORUS, 0], [CC_DELAY, 0],
                    [controller, level]]
        }));
    }
}

/* Channel pressure -> vibLfoToPitch, 50 */
for (const level of MOD_LEVELS) {
    files.set(`grid_pressure-${String(level).padStart(3, "0")}`, cell({
        setup: [[CC_PAN, 64], [CC_REVERB, 0], [CC_CHORUS, 0], [CC_DELAY, 0]],
        pre: [channelPressure(0, CH, level)]
    }));
}

/* Pitch wheel -> fineTune, 12700.  Centre is 8192; the ends are a full
 * bend either way, so the column should be symmetric about the middle. */
for (const [label, value] of [
    ["000", 0],
    ["032", 4096],
    ["064", 8192],
    ["096", 12288],
    ["127", 16383]
]) {
    files.set(`grid_pitchwheel-${label}`, cell({
        setup: [[CC_PAN, 64], [CC_REVERB, 0], [CC_CHORUS, 0], [CC_DELAY, 0]],
        pre: [pitchBend(0, CH, value)]
    }));
}

/* ── Axis: random panning ────────────────────────────────────────────────
 * A GS part pan of 0 selects random panning, so every note-on draws from
 * the synthesizer's generator. Sweeping the note count checks the two
 * engines stay in lockstep across successive draws rather than merely
 * agreeing on the first: a generator that matches for one note but drifts
 * afterwards shows up as a deficit that grows down this column.
 *
 * Both engines seed from the same constant, so with a deterministic
 * generator every cell here should be flat. */
for (const notes of [1, 2, 4, 8, 16]) {
    files.set(`grid_randompan-${String(notes).padStart(3, "0")}`, cell({
        setup: [[CC_REVERB, 0], [CC_CHORUS, 0], [CC_DELAY, 0]],
        sysex: [gs(0, 0x40, 0x11, 0x1c, [0])], // part 1 pan = 0 = random
        noteCount: notes
    }));
}

/* The same note counts without the random pan, so the two axes read against
 * each other: what randompan alone shows is a random pan that disagrees, and
 * what both show is retriggering the same note. Without this control, a walk
 * down randompan indicts the random generator for whatever repeated notes do. */
for (const notes of [1, 2, 4, 8, 16]) {
    files.set(`grid_notecount-${String(notes).padStart(3, "0")}`, cell({
        setup: [[CC_REVERB, 0], [CC_CHORUS, 0], [CC_DELAY, 0]],
        noteCount: notes
    }));
}

const outDir = path.resolve(
    process.argv[2] ??
        path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "grid")
);
await fs.mkdir(outDir, { recursive: true });

for (const [name, data] of files) {
    await fs.writeFile(path.join(outDir, `${name}.mid`), data);
}
console.log(`${files.size} grid probe files in ${outDir}`);
