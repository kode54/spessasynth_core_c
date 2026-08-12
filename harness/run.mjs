#!/usr/bin/env node
/**
 * Render a corpus of MIDI files with both engines and report how they differ.
 *
 *   node harness/run.mjs [options] [midi files or directories...]
 *
 * With no positional arguments the upstream generated test corpus is used
 * (tests/midi_file/generated in the JS repo), falling back to the C repo's
 * examples directory.
 */

import * as fs from "node:fs/promises";
import * as fsSync from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { compareFiles, formatReport } from "./compare.mjs";
import { fmtDb } from "./lib/analysis.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "..");
const JS_REPO = path.resolve(REPO, "../spessasynth_core");
const C_RENDERER = path.join(HERE, "build", "ss_render_c");
const JS_RENDERER = path.join(HERE, "render_js.ts");

const MIDI_EXTENSIONS = new Set([
    ".mid",
    ".midi",
    ".rmi",
    ".kar",
    ".xmi",
    ".mus",
    ".hmi",
    ".hmp",
    ".mids",
    ".xmf"
]);

/*
 * Sound bank used when --sf is not given.
 *
 * Upstream renders its generated corpus against a square.sf2 that is not in
 * the repo and is not fetched by CI — it is a local file on the maintainer's
 * machine — so an exact match is not reproducible here. What the corpus
 * actually needs is full GM/GS coverage: its tests select specific
 * instruments, and examples/florestan-subset.sf2 contains only 17 presets,
 * so most of the corpus lands on fallback presets and measures nothing
 * useful. Point SPESSA_HARNESS_SF at a full bank, or pass --sf.
 */
const FALLBACK_SF = path.join(
    REPO,
    "spessasynth_core",
    "examples",
    "florestan-subset.sf2"
);

/*
 * Same bank, same filename, different base directory per machine. The
 * capitalization is load-bearing on Linux and forgiven on macOS, so it is
 * written once here rather than per path.
 */
const CORPUS_SF_NAME = "TyrolandGS30finalfixed.sf2";
const CORPUS_SF_DIRS = [
    "/Users/Shared/SoundFonts", // macOS development
    "/usr/share/soundfonts" // Linux
];

function defaultSoundBank() {
    if (process.env.SPESSA_HARNESS_SF) {
        return path.resolve(process.env.SPESSA_HARNESS_SF);
    }
    for (const dir of CORPUS_SF_DIRS) {
        const candidate = path.join(dir, CORPUS_SF_NAME);
        if (fsSync.existsSync(candidate)) {
            return candidate;
        }
    }
    return FALLBACK_SF;
}

const DEFAULTS = {
    sf: defaultSoundBank(),
    midiDir: path.join(JS_REPO, "tests", "midi_file", "generated"),
    // Renders land in the repo, not /tmp: /tmp is a tmpfs here and a full
    // corpus run is about a gigabyte.  Both directories are gitignored.
    out: path.join(HERE, "corpus"),
    rate: 48000,
    tail: 2,
    block: 128,
    voiceCap: 350,
    loopCount: 0,
    jobs: Math.max(1, Math.min(8, Math.floor(os.cpus().length / 2))),
    failBelow: null,
    autoAllocate: false,
    effects: true,
    verbose: false,
    generate: false,
    filter: null,
    timeline: false
};

function usage() {
    console.error(`usage: node harness/run.mjs [options] [midi files or dirs...]

  --sf PATH           sound bank to render with
                      (default: $SPESSA_HARNESS_SF, else ${DEFAULTS.sf})
  --out DIR           output directory (default: harness/out)
  --filter SUBSTR     only render files whose name contains SUBSTR
  --gen               run "npm run test:midi" in the JS repo first
  --rate N            sample rate (default ${DEFAULTS.rate})
  --tail S            tail seconds after the song (default ${DEFAULTS.tail})
  --block N           render block size (default ${DEFAULTS.block})
  --voice-cap N       voice cap (default ${DEFAULTS.voiceCap})
  --loop-count N      sequencer loop count (default ${DEFAULTS.loopCount}).
                      The C renderer is run with --loop-play-out so it stops
                      looping when the count runs out instead of fading, which
                      is what upstream does and the only way a looped render
                      is comparable
  --auto-allocate     uncapped voice allocation on both engines
  --no-effects        disable reverb/chorus/delay on both engines
  --jobs N            parallel renders (default ${DEFAULTS.jobs})
  --fail-below DB     exit non-zero if any file scores below this SNR
  --timeline          keep the full per-window timeline in the JSON reports
  --verbose           pass through info-level engine logging`);
    process.exit(2);
}

function parseArgs(argv) {
    const o = { ...DEFAULTS, inputs: [] };
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
            case "--sf": o.sf = path.resolve(value()); break;
            case "--out": o.out = path.resolve(value()); break;
            case "--filter": o.filter = value(); break;
            case "--rate": o.rate = Number(value()); break;
            case "--tail": o.tail = Number(value()); break;
            case "--block": o.block = Number(value()); break;
            case "--voice-cap": o.voiceCap = Number(value()); break;
            case "--loop-count": o.loopCount = Number(value()); break;
            case "--jobs": o.jobs = Math.max(1, Number(value())); break;
            case "--fail-below": o.failBelow = Number(value()); break;
            case "--gen": o.generate = true; break;
            case "--auto-allocate": o.autoAllocate = true; break;
            case "--no-effects": o.effects = false; break;
            case "--timeline": o.timeline = true; break;
            case "--verbose": o.verbose = true; break;
            case "-h":
            case "--help": usage(); break;
            default:
                if (a.startsWith("--")) {
                    console.error(`Unknown option '${a}'`);
                    usage();
                }
                o.inputs.push(a);
        }
    }
    return o;
}

async function exists(p) {
    try {
        await fs.access(p);
        return true;
    } catch {
        return false;
    }
}

async function collectMidis(inputs, filter) {
    const roots = inputs.length > 0 ? inputs : [DEFAULTS.midiDir];
    const files = [];
    for (const root of roots) {
        const resolved = path.resolve(root);
        if (!(await exists(resolved))) {
            console.error(`Skipping missing path: ${resolved}`);
            continue;
        }
        const stat = await fs.stat(resolved);
        if (stat.isDirectory()) {
            for (const entry of await fs.readdir(resolved)) {
                if (MIDI_EXTENSIONS.has(path.extname(entry).toLowerCase())) {
                    files.push(path.join(resolved, entry));
                }
            }
        } else {
            files.push(resolved);
        }
    }
    const filtered = filter
        ? files.filter((f) => path.basename(f).includes(filter))
        : files;
    return filtered.sort();
}

function run(command, args, options = {}) {
    return new Promise((resolve) => {
        const child = spawn(command, args, {
            stdio: ["ignore", "pipe", "pipe"],
            ...options
        });
        let stdout = "";
        let stderr = "";
        child.stdout.on("data", (d) => (stdout += d));
        child.stderr.on("data", (d) => (stderr += d));
        child.on("error", (err) =>
            resolve({ code: -1, stdout, stderr: `${stderr}${err.message}\n` })
        );
        child.on("close", (code) => resolve({ code, stdout, stderr }));
    });
}

async function renderOne(o, midiPath, dirs) {
    const name = path.basename(midiPath, path.extname(midiPath));
    const cWav = path.join(dirs.c, `${name}.wav`);
    const jsWav = path.join(dirs.js, `${name}.wav`);

    const shared = [
        "--rate", String(o.rate),
        "--tail", String(o.tail),
        "--block", String(o.block),
        "--voice-cap", String(o.voiceCap),
        ...(o.autoAllocate ? ["--auto-allocate"] : []),
        ...(o.effects ? [] : ["--no-effects"]),
        ...(o.loopCount ? ["--loop-count", String(o.loopCount)] : [])
    ];

    /*
     * The C sequencer's finite loop count fades out and keeps looping; the JS
     * one stops looping and plays the song out.  That is a deliberate player
     * feature with no upstream equivalent, so it is switched off here rather
     * than compared -- otherwise the last seconds of every looping file are
     * a fade against a tail and score arbitrarily badly.
     */
    const cOnly = o.loopCount ? ["--loop-play-out"] : [];

    const started = Date.now();
    const [cRes, jsRes] = await Promise.all([
        run(C_RENDERER, [...shared, ...cOnly, o.sf, midiPath, cWav]),
        run(
            "tsx",
            [
                JS_RENDERER,
                ...shared,
                ...(o.verbose ? ["--verbose"] : []),
                o.sf,
                midiPath,
                jsWav
            ],
            { cwd: HERE }
        )
    ]);
    const elapsed = Date.now() - started;

    await fs.writeFile(path.join(dirs.logs, `${name}.c.log`), cRes.stderr);
    await fs.writeFile(path.join(dirs.logs, `${name}.js.log`), jsRes.stderr);

    if (cRes.code !== 0 || jsRes.code !== 0) {
        return {
            name,
            midiPath,
            error:
                cRes.code !== 0
                    ? `C renderer failed (exit ${cRes.code}): ${lastLine(cRes.stderr)}`
                    : `JS renderer failed (exit ${jsRes.code}): ${lastLine(jsRes.stderr)}`,
            elapsed
        };
    }

    let report;
    try {
        report = await compareFiles(jsWav, cWav);
    } catch (err) {
        return { name, midiPath, error: `Compare failed: ${err.message}`, elapsed };
    }

    const stored = o.timeline ? report : { ...report, timeline: undefined };
    await fs.writeFile(
        path.join(dirs.reports, `${name}.json`),
        JSON.stringify(stored, null, 2)
    );
    return { name, midiPath, report, elapsed };
}

function lastLine(text) {
    const lines = text.trim().split("\n").filter(Boolean);
    return lines.at(-1) ?? "(no output)";
}

async function pool(items, limit, worker) {
    const results = new Array(items.length);
    let next = 0;
    const runners = Array.from({ length: Math.min(limit, items.length) }, async () => {
        for (;;) {
            const index = next++;
            if (index >= items.length) return;
            results[index] = await worker(items[index], index);
        }
    });
    await Promise.all(runners);
    return results;
}

const VERDICT_ORDER = [
    "major",
    "moderate",
    "minor",
    "very-close",
    "near-identical",
    "identical"
];

function summarize(results) {
    const ok = results.filter((r) => r.report);
    const failed = results.filter((r) => r.error);
    const rows = ok
        .map((r) => ({
            name: r.name,
            verdict: r.report.verdict,
            snr: r.report.headlineSnrDb,
            rawSnr: r.report.overall.snrDb,
            corr: r.report.overall.worstWindowCorrelation,
            medianCorr: r.report.overall.medianWindowCorrelation,
            globalCorr: r.report.overall.correlation,
            gainDb: r.report.overall.bestFitGainDb,
            lag: r.report.overall.lagSamples,
            driftCents: r.report.overall.driftCents,
            firstDiv: r.report.overall.firstDivergenceSec,
            seconds: r.report.seconds,
            notes: r.report.notes
        }))
        .sort((a, b) => a.snr - b.snr);

    const counts = {};
    for (const row of rows) {
        counts[row.verdict] = (counts[row.verdict] ?? 0) + 1;
    }
    return { rows, failed, counts };
}

function renderMarkdown(summary, o, midis) {
    const lines = [];
    lines.push("# spessasynth_core_c vs spessasynth_core render comparison");
    lines.push("");
    lines.push(`- Sound bank: \`${o.sf}\``);
    lines.push(
        `- ${o.rate} Hz, block ${o.block}, tail ${o.tail} s, voice cap ${o.voiceCap}` +
            `, effects ${o.effects ? "on" : "off"}` +
            `, auto-allocate ${o.autoAllocate ? "on" : "off"}` +
            (o.loopCount ? `, loop count ${o.loopCount}` : "")
    );
    lines.push(`- Files: ${midis.length}`);
    lines.push("");
    lines.push("## Verdict counts");
    lines.push("");
    lines.push("| Verdict | Files |");
    lines.push("| --- | ---: |");
    for (const v of VERDICT_ORDER) {
        if (summary.counts[v]) {
            lines.push(`| ${v} | ${summary.counts[v]} |`);
        }
    }
    if (summary.failed.length > 0) {
        lines.push(`| render/compare error | ${summary.failed.length} |`);
    }
    lines.push("");
    lines.push("## Per-file results (worst first)");
    lines.push("");
    lines.push(
        "| File | Verdict | SNR dB (aligned) | SNR dB (raw) | Worst window corr | Gain dB | Lag | Drift ¢ | First div. (s) |"
    );
    lines.push("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |");
    for (const row of summary.rows) {
        lines.push(
            `| ${row.name} | ${row.verdict} | ${fmtDb(row.snr)} | ${fmtDb(row.rawSnr)} | ` +
                `${row.corr ?? "n/a"} | ${fmtDb(row.gainDb)} | ${row.lag} | ` +
                `${row.driftCents ?? 0} | ${row.firstDiv ?? "0"} |`
        );
    }

    const withNotes = summary.rows.filter((r) => r.notes.length > 0);
    if (withNotes.length > 0) {
        lines.push("");
        lines.push("## Notes");
        lines.push("");
        for (const row of withNotes) {
            lines.push(`### ${row.name}`);
            for (const note of row.notes) {
                lines.push(`- ${note}`);
            }
            lines.push("");
        }
    }

    if (summary.failed.length > 0) {
        lines.push("## Errors");
        lines.push("");
        for (const f of summary.failed) {
            lines.push(`- **${f.name}**: ${f.error}`);
        }
        lines.push("");
    }
    return lines.join("\n");
}

/* ── main ───────────────────────────────────────────────────────────────── */

const o = parseArgs(process.argv.slice(2));

if (!(await exists(C_RENDERER))) {
    console.error(
        `C renderer not built. Run:\n  ${path.join(HERE, "build.sh")}`
    );
    process.exit(1);
}
if (!(await exists(o.sf))) {
    console.error(`Sound bank not found: ${o.sf}`);
    process.exit(1);
}
if (!(await exists(JS_REPO))) {
    console.error(`Upstream JS repo not found at ${JS_REPO}`);
    process.exit(1);
}

if (o.generate) {
    console.error("Generating upstream test MIDI files...");
    const res = await run("npm", ["run", "test:midi"], { cwd: JS_REPO });
    if (res.code !== 0) {
        console.error(res.stderr || res.stdout);
        console.error("Test file generation failed.");
        process.exit(1);
    }
}

const midis = await collectMidis(o.inputs, o.filter);
if (midis.length === 0) {
    console.error(
        "No MIDI files found. Pass paths explicitly, or run with --gen to " +
            "generate the upstream test corpus."
    );
    process.exit(1);
}

const dirs = {
    c: path.join(o.out, "c"),
    js: path.join(o.out, "js"),
    logs: path.join(o.out, "logs"),
    reports: path.join(o.out, "reports")
};
for (const dir of Object.values(dirs)) {
    await fs.mkdir(dir, { recursive: true });
}

/*
 * Always say which bank was resolved, and say loudly when it is the in-repo
 * subset: that fallback still renders and still scores, but against 17
 * presets, so the numbers look comparable to a full-coverage run and are not.
 * A silent fallback is the one failure here that produces wrong answers
 * rather than no answers.
 */
console.error(`Sound bank: ${o.sf}`);
if (path.resolve(o.sf) === path.resolve(FALLBACK_SF)) {
    console.error(
        "  WARNING: this is the in-repo 17-preset subset, not a full GM/GS bank.\n" +
            "  Most of the corpus will land on fallback presets, so the results\n" +
            "  measure preset resolution rather than synthesis and are NOT\n" +
            "  comparable to a full-bank run. Set SPESSA_HARNESS_SF or pass --sf."
    );
}

console.error(
    `Rendering ${midis.length} file(s) with both engines (${o.jobs} parallel)...`
);

let done = 0;
const results = await pool(midis, o.jobs, async (midiPath) => {
    const result = await renderOne(o, midiPath, dirs);
    done++;
    const status = result.error
        ? `ERROR ${result.error}`
        : `${result.report.verdict} (SNR ${fmtDb(result.report.headlineSnrDb)} dB)`;
    console.error(`[${done}/${midis.length}] ${result.name}: ${status}`);
    return result;
});

const summary = summarize(results);

console.log("");
for (const r of results) {
    if (r.report) {
        console.log(formatReport(r.report, r.name));
        console.log("");
    }
}

console.log("=".repeat(72));
for (const v of VERDICT_ORDER) {
    if (summary.counts[v]) {
        console.log(`${v.padEnd(16)} ${summary.counts[v]}`);
    }
}
if (summary.failed.length > 0) {
    console.log(`${"errors".padEnd(16)} ${summary.failed.length}`);
    for (const f of summary.failed) {
        console.log(`  ${f.name}: ${f.error}`);
    }
}

const markdown = renderMarkdown(summary, o, midis);
const summaryPath = path.join(o.out, "summary.md");
await fs.writeFile(summaryPath, markdown);
await fs.writeFile(
    path.join(o.out, "summary.json"),
    JSON.stringify({ options: o, rows: summary.rows, failed: summary.failed }, null, 2)
);
console.log(`\nWrote ${summaryPath}`);

let exitCode = summary.failed.length > 0 ? 1 : 0;
if (o.failBelow !== null) {
    const below = summary.rows.filter((r) => r.snr < o.failBelow);
    if (below.length > 0) {
        console.error(
            `\n${below.length} file(s) below the ${o.failBelow} dB SNR threshold: ` +
                below.map((r) => r.name).join(", ")
        );
        exitCode = 1;
    }
}
process.exit(exitCode);
