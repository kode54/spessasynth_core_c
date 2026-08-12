/**
 * Offline WAV renderer for upstream spessasynth_core (TypeScript).
 *
 * Mirrors harness/render_c.c step for step: identical sample rate, block
 * size and total sample count, sequencer tick before every render block,
 * no looping, no normalization, 32-bit float output.
 *
 * Run with: tsx render_js.ts [options] <soundbank> <midi> <out.wav>
 */

import * as fs from "node:fs/promises";
import * as path from "node:path";
import {
    BasicMIDI,
    SoundBankLoader,
    SpessaLog,
    SpessaSynthProcessor,
    SpessaSynthSequencer
} from "../../spessasynth_core/src";
import { writeWavF32 } from "./lib/wav.mjs";

const DEF_RATE = 48_000;
const DEF_TAIL = 2;
const DEF_BLOCK = 128;
const DEF_VOICE_CAP = 350;

interface Options {
    rate: number;
    tail: number;
    block: number;
    voiceCap: number;
    autoAllocate: boolean;
    effects: boolean;
    loopCount: number;
    emidiFilter: boolean;
    verbose: boolean;
    noSkip: boolean;
    bankPath: string;
    midiPath: string;
    outPath: string;
}

function usage(): never {
    console.error(
        `usage: tsx render_js.ts [options] <soundbank> <midi> <out.wav>

  --rate N            sample rate (default ${DEF_RATE})
  --tail S            seconds of tail after the song (default ${DEF_TAIL})
  --block N           render block size (default ${DEF_BLOCK})
  --voice-cap N       voice cap (default ${DEF_VOICE_CAP})
  --auto-allocate     uncapped voice allocation (default off)
  --no-effects        disable reverb/chorus/delay
  --loop-count N      sequencer loop count (default 0, i.e. no loop)
  --emidi-filter      drop EMIDI tracks designated for non-GM devices
  --verbose           enable info-level logging from the core`
    );
    process.exit(2);
}

function parseArgs(argv: string[]): Options {
    const o: Options = {
        rate: DEF_RATE,
        tail: DEF_TAIL,
        block: DEF_BLOCK,
        voiceCap: DEF_VOICE_CAP,
        autoAllocate: false,
        effects: true,
        loopCount: 0,
        emidiFilter: false,
        verbose: false,
        noSkip: false,
        bankPath: "",
        midiPath: "",
        outPath: ""
    };
    const positional: string[] = [];

    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        const value = () => {
            const v = argv[++i];
            if (v === undefined) {
                console.error(`${a} requires a value`);
                process.exit(2);
            }
            return v;
        };
        switch (a) {
            case "--rate":
                o.rate = Number(value());
                break;
            case "--tail":
                o.tail = Number(value());
                break;
            case "--block":
                o.block = Number(value());
                break;
            case "--voice-cap":
                o.voiceCap = Number(value());
                break;
            case "--loop-count":
                o.loopCount = Number(value());
                break;
            case "--emidi-filter":
                o.emidiFilter = true;
                break;
            case "--auto-allocate":
                o.autoAllocate = true;
                break;
            case "--no-effects":
                o.effects = false;
                break;
            case "--verbose":
                o.verbose = true;
                break;
            case "--no-skip":
                o.noSkip = true;
                break;
            case "-h":
            case "--help":
                usage();
                break;
            default:
                if (a.startsWith("--")) {
                    console.error(`Unknown option '${a}'`);
                    process.exit(2);
                }
                positional.push(a);
        }
    }

    if (positional.length !== 3 || !o.rate || !o.block) {
        usage();
    }
    [o.bankPath, o.midiPath, o.outPath] = positional;
    return o;
}

const o = parseArgs(process.argv.slice(2));

// Route all core logging to stderr so stdout stays clean and the harness can
// diff the two engines' warnings side by side.
SpessaLog.setLogLevel(o.verbose, true, false);
const toStderr = (...parts: unknown[]) => {
    const text = parts
        .map((p) => String(p).replaceAll("%c", ""))
        .filter((p) => !p.includes("color: "))
        .join(" ")
        .trim();
    if (text.length > 0) {
        process.stderr.write(`${text}\n`);
    }
};
SpessaLog.logFunctions = {
    info: toStderr,
    warn: toStderr,
    group: () => undefined,
    groupCollapsed: () => undefined,
    groupEnd: () => undefined
};

const bankBin = await fs.readFile(o.bankPath);
const midiBin = await fs.readFile(o.midiPath);
const soundBank = SoundBankLoader.fromArrayBuffer(
    bankBin.buffer.slice(
        bankBin.byteOffset,
        bankBin.byteOffset + bankBin.byteLength
    ) as ArrayBuffer
);
const midi = BasicMIDI.fromArrayBuffer(
    midiBin.buffer.slice(
        midiBin.byteOffset,
        midiBin.byteOffset + midiBin.byteLength
    ) as ArrayBuffer
);
if (o.emidiFilter) {
    midi.removeEMIDINonGMTracks();
}

const synth = new SpessaSynthProcessor(o.rate, {
    eventsEnabled: false,
    maxBufferSize: o.block
});
synth.soundBankManager.addSoundBank(soundBank, "main");
await synth.processorInitialized;
synth.setSystemParameter("autoAllocateVoices", o.autoAllocate);
synth.setSystemParameter("effectsEnabled", o.effects);
synth.setSystemParameter("voiceCap", o.voiceCap);

const seq = new SpessaSynthSequencer(synth);
seq.skipToFirstNoteOn = !o.noSkip;
seq.loadNewSongList([midi]);
seq.loopCount = o.loopCount;
seq.play();

const frames = Math.ceil(o.rate * (midi.duration + o.tail));
if (frames === 0) {
    console.error(`Nothing to render (duration ${midi.duration})`);
    process.exit(1);
}

process.stderr.write(
    `[js] ${path.basename(o.midiPath)}: duration ${midi.duration.toFixed(3)} s, ` +
        `rendering ${frames} frames at ${o.rate} Hz\n`
);

const left = new Float32Array(frames);
const right = new Float32Array(frames);

let filled = 0;
while (filled < frames) {
    seq.processTick();
    const n = Math.min(o.block, frames - filled);
    synth.process(left, right, filled, n);
    filled += n;
}

await fs.writeFile(o.outPath, writeWavF32([left, right], o.rate));
process.stderr.write(`[js] wrote ${o.outPath}\n`);
