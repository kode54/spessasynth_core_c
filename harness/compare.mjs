#!/usr/bin/env node
/**
 * Compare two renders of the same MIDI file.
 *
 * Usage: node compare.mjs <reference.wav> <candidate.wav> [--json] [--timeline]
 *
 * "reference" is upstream (JS) by convention, "candidate" is the C port, so a
 * positive gain/lag figure describes what the C port does relative to upstream.
 */

import * as fs from "node:fs/promises";
import * as path from "node:path";
import { readWav } from "./lib/wav.mjs";
import {
    averageSpectrum,
    bestFitGain,
    countNonFinite,
    estimateLag,
    fmtDb,
    lagDrift,
    lagTrack,
    trackedSnr,
    octaveBandDelta,
    peak,
    pearson,
    residualRms,
    rms,
    round,
    toDb,
    toMono
} from "./lib/analysis.mjs";

// A difference below this is inaudible and not worth flagging as a divergence
// point; it is roughly -80 dBFS.
const DIVERGENCE_THRESHOLD = 1e-4;
const WINDOW_SECONDS = 0.1;

export function compareRenders(refWav, candWav, opts = {}) {
    const threshold = opts.threshold ?? DIVERGENCE_THRESHOLD;
    const windowSeconds = opts.windowSeconds ?? WINDOW_SECONDS;

    if (refWav.sampleRate !== candWav.sampleRate) {
        throw new Error(
            `Sample rate mismatch: ${refWav.sampleRate} vs ${candWav.sampleRate}`
        );
    }
    const sampleRate = refWav.sampleRate;
    const a = refWav.channels;
    const b = candWav.channels;
    if (a.length !== b.length) {
        throw new Error(
            `Channel count mismatch: ${a.length} vs ${b.length}`
        );
    }

    const framesA = a[0].length;
    const framesB = b[0].length;
    const n = Math.min(framesA, framesB);

    const notes = [];
    if (framesA !== framesB) {
        notes.push(
            `Length differs: reference ${framesA} frames, candidate ${framesB} ` +
                `(${round(((framesB - framesA) / sampleRate) * 1000, 1)} ms)`
        );
    }

    const nonFiniteA = a.reduce((s, ch) => s + countNonFinite(ch), 0);
    const nonFiniteB = b.reduce((s, ch) => s + countNonFinite(ch), 0);
    if (nonFiniteA > 0) notes.push(`Reference contains ${nonFiniteA} non-finite samples`);
    if (nonFiniteB > 0) notes.push(`Candidate contains ${nonFiniteB} non-finite samples`);

    // Mono-summed figures drive the headline verdict and the lag/gain fit.
    const monoA = toMono(a, n);
    const monoB = toMono(b, n);
    const rmsMonoA = rms(monoA);
    const rmsMonoB = rms(monoB);
    const rmsDiff = residualRms(monoA, monoB, n);
    const snrDb = toDb(rmsMonoA / (rmsDiff || Number.MIN_VALUE));

    // A constant offset (e.g. one render block of engine latency) would
    // otherwise swamp every other metric, so everything below is measured
    // after removing it.
    const lag = opts.skipLag ? 0 : estimateLag(monoA, monoB, sampleRate);
    const rmsDiffLag = residualRms(monoA, monoB, n, lag);
    const snrAfterLagDb = toDb(rmsMonoA / (rmsDiffLag || Number.MIN_VALUE));

    const gain = bestFitGain(monoA, monoB, n, lag);
    const rmsDiffGain = residualRms(monoA, monoB, n, lag, gain);
    const snrAfterGainDb = toDb((rmsMonoA * Math.abs(gain)) / (rmsDiffGain || Number.MIN_VALUE));

    // Does the offset hold, or does it walk? A walking offset means the two
    // engines run at slightly different playback rates.
    const track = opts.skipLag ? [] : lagTrack(monoA, monoB, sampleRate);
    const drift = lagDrift(track, sampleRate);
    const trackedSnrDb =
        track.length > 0 ? trackedSnr(monoA, monoB, sampleRate, track) : -Infinity;
    const windowCorrelations = track
        .map((t) => t.correlation)
        .sort((x, y) => x - y);
    const medianWindowCorrelation =
        windowCorrelations.length > 0
            ? windowCorrelations[windowCorrelations.length >> 1]
            : null;

    // Per-channel figures, measured at the aligned offset.
    const channels = [];
    let bitExact = framesA === framesB;
    const from = Math.max(0, -lag);
    const to = Math.min(n, n - lag);
    for (let c = 0; c < a.length; c++) {
        const ca = a[c];
        const cb = b[c];
        let maxDiff = 0;
        let maxDiffAt = 0;
        let firstDiverge = -1;
        for (let i = from; i < to; i++) {
            const d = Math.abs(cb[i + lag] - ca[i]);
            if (d !== 0) bitExact = false;
            if (d > maxDiff) {
                maxDiff = d;
                maxDiffAt = i;
            }
            if (firstDiverge < 0 && d > threshold) {
                firstDiverge = i;
            }
        }
        if (lag !== 0) bitExact = false;
        const rmsA = rms(ca, from, to);
        const rmsB = rms(cb, from + lag, to + lag);
        const rmsChDiff = residualRms(ca, cb, n, lag);
        channels.push({
            channel: c,
            peakA: round(peak(ca), 6),
            peakB: round(peak(cb), 6),
            peakADbfs: round(toDb(peak(ca)), 2),
            peakBDbfs: round(toDb(peak(cb)), 2),
            rmsADbfs: round(toDb(rmsA), 2),
            rmsBDbfs: round(toDb(rmsB), 2),
            maxAbsDiff: round(maxDiff, 6),
            maxAbsDiffDbfs: round(toDb(maxDiff), 2),
            maxAbsDiffAtSec: round(maxDiffAt / sampleRate, 4),
            rmsDiffDbfs: round(toDb(rmsChDiff), 2),
            snrDb: round(toDb(rmsA / (rmsChDiff || Number.MIN_VALUE)), 2),
            correlation: round(pearson(ca, cb, n), 6),
            firstDivergenceSec:
                firstDiverge < 0 ? null : round(firstDiverge / sampleRate, 4)
        });
    }

    // Where does the difference sit in the timeline?
    const windowFrames = Math.max(1, Math.round(windowSeconds * sampleRate));
    const timeline = [];
    for (let start = from; start < to; start += windowFrames) {
        const end = Math.min(to, start + windowFrames);
        const wa = rms(monoA, start, end);
        const wb = rms(monoB, start + lag, end + lag);
        const wd = residualRms(
            monoA.subarray(start, end),
            monoB.subarray(start + lag, end + lag),
            end - start
        );
        timeline.push({
            tSec: round(start / sampleRate, 3),
            rmsADbfs: round(toDb(wa), 2),
            rmsBDbfs: round(toDb(wb), 2),
            rmsDiffDbfs: round(toDb(wd), 2),
            snrDb: round(toDb(wa / (wd || Number.MIN_VALUE)), 2)
        });
    }
    // Windows where both engines are essentially silent carry no information.
    const worstWindows = timeline
        .filter((w) => w.rmsADbfs > -80 || w.rmsBDbfs > -80)
        .sort((x, y) => x.snrDb - y.snrDb)
        .slice(0, 10);

    let bands = [];
    if (!opts.skipSpectrum && n >= 4096) {
        bands = octaveBandDelta(
            averageSpectrum(monoA),
            averageSpectrum(monoB),
            sampleRate
        );
    }

    const firstDivergenceSec = channels
        .map((c) => c.firstDivergenceSec)
        .filter((t) => t !== null)
        .sort((x, y) => x - y)[0];

    // Interpretation.
    const gainDb = toDb(Math.abs(gain));
    if (rmsMonoA === 0 && rmsMonoB > 0) {
        notes.push("Reference is silent but the candidate is not");
    } else if (rmsMonoB === 0 && rmsMonoA > 0) {
        notes.push("Candidate is silent but the reference is not");
    }
    if (Math.abs(gainDb) > 0.05 && snrAfterGainDb - snrDb > 3) {
        notes.push(
            `Mostly a level difference: candidate is ${round(gainDb, 2)} dB ` +
                `relative to reference (SNR improves to ${round(snrAfterGainDb, 1)} dB once corrected)`
        );
    }
    if (lag !== 0) {
        notes.push(
            `Candidate is offset by ${lag} samples (${round((lag / sampleRate) * 1000, 3)} ms` +
                `${lag % 128 === 0 ? `, exactly ${Math.abs(lag) / 128} render block(s)` : ""}); ` +
                `raw SNR ${fmtDb(snrDb)} dB, ${fmtDb(snrAfterLagDb)} dB once aligned`
        );
    }
    if (Math.abs(drift.cents) >= 0.05) {
        notes.push(
            `Playback rate differs: alignment drifts ${round(drift.samplesPerSecond, 3)} ` +
                `samples/s, i.e. the candidate is ${round(drift.cents, 3)} cents ` +
                `${drift.cents < 0 ? "flat" : "sharp"} relative to the reference ` +
                `(SNR ${fmtDb(trackedSnrDb)} dB once the drift is tracked out)`
        );
    }
    if (medianWindowCorrelation !== null && medianWindowCorrelation < 0.9) {
        notes.push(
            `Even window-by-window the waveforms only correlate to ` +
                `${round(medianWindowCorrelation, 4)} — a genuine synthesis difference`
        );
    }
    if (firstDivergenceSec !== undefined && firstDivergenceSec > 0.01) {
        notes.push(
            `Matches for the first ${firstDivergenceSec} s (after alignment), then diverges`
        );
    }
    for (const band of bands) {
        if (Math.abs(band.deltaDb) >= 1 && Math.max(band.aDb, band.bDb) > -90) {
            notes.push(
                `${band.loHz}–${band.hiHz} Hz band differs by ${band.deltaDb} dB`
            );
        }
    }

    // Graded on the drift-corrected SNR: a constant engine-latency offset is
    // a property of the driver rather than of synthesis, and a slow rate drift
    // is reported separately (in cents) instead of being allowed to masquerade
    // as a large waveform difference.
    const headlineSnr = Math.max(snrDb, snrAfterLagDb, trackedSnrDb);
    const verdict = bitExact
        ? "identical"
        : headlineSnr >= 90
          ? "near-identical"
          : headlineSnr >= 60
            ? "very-close"
            : headlineSnr >= 30
              ? "minor"
              : headlineSnr >= 12
                ? "moderate"
                : "major";

    return {
        sampleRate,
        frames: { reference: framesA, candidate: framesB, compared: n },
        seconds: {
            reference: round(framesA / sampleRate, 3),
            candidate: round(framesB / sampleRate, 3)
        },
        bitExact,
        verdict,
        headlineSnrDb: round(headlineSnr, 2),
        overall: {
            rmsADbfs: round(toDb(rmsMonoA), 2),
            rmsBDbfs: round(toDb(rmsMonoB), 2),
            peakADbfs: round(toDb(peak(monoA)), 2),
            peakBDbfs: round(toDb(peak(monoB)), 2),
            rmsDiffDbfs: round(toDb(rmsDiff), 2),
            snrDb: round(snrDb, 2),
            correlation: round(pearson(monoA, monoB, n), 6),
            bestFitGain: round(gain, 6),
            bestFitGainDb: round(gainDb, 3),
            snrAfterGainDb: round(snrAfterGainDb, 2),
            lagSamples: lag,
            lagMs: round((lag / sampleRate) * 1000, 4),
            snrAfterLagDb: round(snrAfterLagDb, 2),
            trackedSnrDb: round(trackedSnrDb, 2),
            medianWindowCorrelation: round(medianWindowCorrelation, 6),
            driftSamplesPerSec: round(drift.samplesPerSecond, 4),
            driftCents: round(drift.cents, 4),
            firstDivergenceSec: firstDivergenceSec ?? null,
            nonFiniteReference: nonFiniteA,
            nonFiniteCandidate: nonFiniteB
        },
        channels,
        bands,
        worstWindows,
        timeline,
        lagTrack: track.map((t) => ({
            tSec: round(t.tSec, 3),
            lag: t.lag,
            correlation: round(t.correlation, 5)
        })),
        notes
    };
}

export async function compareFiles(refPath, candPath, opts = {}) {
    const [refBin, candBin] = await Promise.all([
        fs.readFile(refPath),
        fs.readFile(candPath)
    ]);
    return compareRenders(readWav(refBin), readWav(candBin), opts);
}

export function formatReport(report, label = "") {
    const o = report.overall;
    const lines = [];
    const head = label ? `${label}: ` : "";
    lines.push(
        `${head}${report.verdict.toUpperCase()}  SNR ${fmtDb(report.headlineSnrDb)} dB` +
            `${o.lagSamples !== 0 ? ` (aligned; raw ${fmtDb(o.snrDb)} dB)` : ""}  ` +
            `window-corr ${o.medianWindowCorrelation ?? "n/a"}  ` +
            `gain ${fmtDb(o.bestFitGainDb)} dB  lag ${o.lagSamples} smp  ` +
            `drift ${o.driftCents ?? 0} ¢`
    );
    lines.push(
        `  reference ${fmtDb(o.rmsADbfs)} dBFS RMS / ${fmtDb(o.peakADbfs)} dBFS peak, ` +
            `candidate ${fmtDb(o.rmsBDbfs)} dBFS RMS / ${fmtDb(o.peakBDbfs)} dBFS peak, ` +
            `${report.seconds.reference}s vs ${report.seconds.candidate}s`
    );
    for (const c of report.channels) {
        lines.push(
            `  ch${c.channel}: SNR ${fmtDb(c.snrDb)} dB, max |diff| ${fmtDb(c.maxAbsDiffDbfs)} dBFS ` +
                `at ${c.maxAbsDiffAtSec}s, first divergence ${c.firstDivergenceSec ?? "none"}`
        );
    }
    if (report.worstWindows.length > 0) {
        const w = report.worstWindows
            .slice(0, 5)
            .map((x) => `${x.tSec}s (${fmtDb(x.snrDb)} dB)`)
            .join(", ");
        lines.push(`  worst windows: ${w}`);
    }
    for (const note of report.notes) {
        lines.push(`  · ${note}`);
    }
    return lines.join("\n");
}

/* ── CLI ────────────────────────────────────────────────────────────────── */

const isMain =
    process.argv[1] &&
    path.resolve(process.argv[1]) === path.resolve(new URL(import.meta.url).pathname);

if (isMain) {
    const args = process.argv.slice(2);
    const flags = new Set(args.filter((a) => a.startsWith("--")));
    const files = args.filter((a) => !a.startsWith("--"));
    if (files.length !== 2) {
        console.error(
            "usage: node compare.mjs <reference.wav> <candidate.wav> [--json] [--timeline]"
        );
        process.exit(2);
    }
    const report = await compareFiles(files[0], files[1], {
        skipSpectrum: flags.has("--no-spectrum"),
        skipLag: flags.has("--no-lag")
    });
    if (flags.has("--json")) {
        const out = flags.has("--timeline")
            ? report
            : { ...report, timeline: undefined };
        console.log(JSON.stringify(out, null, 2));
    } else {
        console.log(formatReport(report, path.basename(files[1])));
    }
    process.exit(report.verdict === "major" ? 1 : 0);
}
