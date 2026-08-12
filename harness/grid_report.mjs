#!/usr/bin/env node
/**
 * Reads a rendered grid corpus as tables, one per axis.
 *
 *   node harness/run.mjs grid --out out-grid
 *   node harness/grid_report.mjs out-grid
 *
 * The point is to read down a column, not across a row. A deficit that stays
 * flat along an axis exonerates that path; one that tracks the parameter
 * indicts it; and the two-axis grid separates an interaction from either axis
 * on its own.
 *
 * Level is reported per channel as well as in total, because a pan law that
 * loses energy off centre shows as a total-power deficit while each channel
 * on its own can look reasonable.
 */

import * as fs from "node:fs/promises";
import * as path from "node:path";
import { readWav } from "./lib/wav.mjs";
import { fmtDb, rms, toDb } from "./lib/analysis.mjs";

const outDir = path.resolve(process.argv[2] ?? "out-grid");

function parseName(name) {
    // grid_pan-064_send-none  ->  { axes: [["pan", 64], ["send", NaN]] }
    const parts = name.replace(/^grid2?_/, "").split("_");
    const axes = [];
    for (const p of parts) {
        const m = /^([a-z]+)-(.+)$/.exec(p);
        if (m) axes.push([m[1], Number(m[2])]);
    }
    return { grid2: name.startsWith("grid2_"), axes };
}

async function loadPair(name) {
    const [a, b] = await Promise.all([
        fs.readFile(path.join(outDir, "js", `${name}.wav`)),
        fs.readFile(path.join(outDir, "c", `${name}.wav`))
    ]);
    return { ref: readWav(a), cand: readWav(b) };
}

/** Total power and per-channel level, in dB, plus the candidate's deficit. */
function levels({ ref, cand }) {
    const stat = (w) => {
        const l = rms(w.channels[0]);
        const r = rms(w.channels[1]);
        const total = Math.sqrt(l * l + r * r);
        return { l, r, total, balance: toDb(r) - toDb(l) };
    };
    const A = stat(ref);
    const B = stat(cand);
    return {
        refTotal: toDb(A.total),
        candTotal: toDb(B.total),
        deficit: toDb(B.total) - toDb(A.total),
        refBalance: A.balance,
        candBalance: B.balance,
        balanceDelta: B.balance - A.balance
    };
}

const entries = [];
for (const file of (await fs.readdir(path.join(outDir, "js"))).sort()) {
    if (!file.endsWith(".wav")) continue;
    const name = path.basename(file, ".wav");
    if (!name.startsWith("grid")) continue;
    const pair = await loadPair(name);
    const l = levels(pair);
    let snr = null;
    try {
        const report = JSON.parse(
            await fs.readFile(path.join(outDir, "reports", `${name}.json`), "utf8")
        );
        snr = report.headlineSnrDb;
    } catch {
        /* report optional */
    }
    entries.push({ name, ...parseName(name), ...l, snr });
}

if (entries.length === 0) {
    console.error(
        `No grid renders under ${outDir}. Generate and render first:\n` +
            "  node tools/make_grid_probes.mjs\n" +
            `  node run.mjs grid --out ${path.basename(outDir)}`
    );
    process.exit(1);
}

/* Single-axis tables: every file whose first axis is X and whose remaining
 * axes are constant across the group. */
const singles = entries.filter((e) => !e.grid2);
const byAxis = new Map();
for (const e of singles) {
    const axis = e.axes[0]?.[0];
    if (!axis) continue;
    if (!byAxis.has(axis)) byAxis.set(axis, []);
    byAxis.get(axis).push(e);
}

const pad = (s, n) => String(s).padStart(n);

for (const [axis, rows] of byAxis) {
    rows.sort((a, b) => a.axes[0][1] - b.axes[0][1]);
    console.log(`\n## ${axis}`);
    console.log(
        `${pad(axis, 8)} ${pad("ref dB", 9)} ${pad("cand dB", 9)} ` +
            `${pad("deficit", 9)} ${pad("bal ref", 9)} ${pad("bal cand", 9)} ${pad("SNR dB", 9)}`
    );
    for (const r of rows) {
        console.log(
            `${pad(r.axes[0][1], 8)} ${pad(fmtDb(r.refTotal), 9)} ` +
                `${pad(fmtDb(r.candTotal), 9)} ${pad(fmtDb(r.deficit), 9)} ` +
                `${pad(fmtDb(r.refBalance), 9)} ${pad(fmtDb(r.candBalance), 9)} ` +
                `${pad(r.snr === null ? "-" : fmtDb(r.snr), 9)}`
        );
    }
    const deficits = rows.map((r) => r.deficit).filter(Number.isFinite);
    if (deficits.length > 1) {
        const spread = Math.max(...deficits) - Math.min(...deficits);
        console.log(
            `  deficit spread across the axis: ${spread.toFixed(2)} dB — ` +
                (spread < 0.1
                    ? "flat, this path is not implicated"
                    : "varies with the parameter, this path is implicated")
        );
    }
}

/* Two-axis grid: rows are the first axis, columns the second, cells the
 * deficit, plus the difference the second axis makes. */
const grids = entries.filter((e) => e.grid2 && e.axes.length >= 2);
if (grids.length > 0) {
    const rowAxis = grids[0].axes[0][0];
    const colAxis = grids[0].axes[1][0];
    const rowVals = [...new Set(grids.map((g) => g.axes[0][1]))].sort((a, b) => a - b);
    const colVals = [...new Set(grids.map((g) => g.axes[1][1]))].sort((a, b) => a - b);
    console.log(`\n## ${rowAxis} x ${colAxis} (deficit dB)`);
    console.log(
        `${pad(rowAxis, 8)} ${colVals.map((c) => pad(`${colAxis} ${c}`, 12)).join(" ")} ${pad("difference", 12)}`
    );
    for (const rv of rowVals) {
        const cells = colVals.map((cv) =>
            grids.find((g) => g.axes[0][1] === rv && g.axes[1][1] === cv)
        );
        const vals = cells.map((c) => (c ? c.deficit : NaN));
        const diff =
            Number.isFinite(vals[0]) && Number.isFinite(vals.at(-1))
                ? vals.at(-1) - vals[0]
                : NaN;
        console.log(
            `${pad(rv, 8)} ${vals.map((v) => pad(fmtDb(v), 12)).join(" ")} ${pad(fmtDb(diff), 12)}`
        );
    }
    console.log(
        "  Read the last column: if it is constant, the two axes are\n" +
            "  independent. If it tracks the row axis, they interact."
    );
}
console.log("");
