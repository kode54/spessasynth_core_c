/**
 * Signal analysis used to characterize the difference between two renders of
 * the same MIDI file.
 *
 * The goal is not a pass/fail bit compare — two independent implementations of
 * a synth will never be bit-identical — but a description of *how* they differ:
 * level, timing, spectrum, and where in the timeline the divergence starts.
 */

const MIN_DB = -200;

export function toDb(x) {
    return x > 0 ? 20 * Math.log10(x) : MIN_DB;
}

export function rms(buf, from = 0, to = buf.length) {
    let sum = 0;
    for (let i = from; i < to; i++) {
        sum += buf[i] * buf[i];
    }
    const n = Math.max(1, to - from);
    return Math.sqrt(sum / n);
}

export function peak(buf) {
    let m = 0;
    for (let i = 0; i < buf.length; i++) {
        const v = Math.abs(buf[i]);
        if (v > m) m = v;
    }
    return m;
}

export function countNonFinite(buf) {
    let n = 0;
    for (let i = 0; i < buf.length; i++) {
        if (!Number.isFinite(buf[i])) n++;
    }
    return n;
}

/** Sum of channels into a single mono buffer (used for spectrum/lag work). */
export function toMono(channels, length) {
    const out = new Float32Array(length);
    for (const ch of channels) {
        for (let i = 0; i < length; i++) {
            out[i] += ch[i];
        }
    }
    if (channels.length > 1) {
        for (let i = 0; i < length; i++) {
            out[i] /= channels.length;
        }
    }
    return out;
}

/* ── in-place iterative radix-2 FFT ─────────────────────────────────────── */

function fft(re, im) {
    const n = re.length;
    for (let i = 1, j = 0; i < n; i++) {
        let bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            [re[i], re[j]] = [re[j], re[i]];
            [im[i], im[j]] = [im[j], im[i]];
        }
    }
    for (let len = 2; len <= n; len <<= 1) {
        const ang = (-2 * Math.PI) / len;
        const wRe = Math.cos(ang);
        const wIm = Math.sin(ang);
        for (let i = 0; i < n; i += len) {
            let curRe = 1;
            let curIm = 0;
            for (let j = 0; j < len / 2; j++) {
                const uRe = re[i + j];
                const uIm = im[i + j];
                const vRe = re[i + j + len / 2] * curRe - im[i + j + len / 2] * curIm;
                const vIm = re[i + j + len / 2] * curIm + im[i + j + len / 2] * curRe;
                re[i + j] = uRe + vRe;
                im[i + j] = uIm + vIm;
                re[i + j + len / 2] = uRe - vRe;
                im[i + j + len / 2] = uIm - vIm;
                const nextRe = curRe * wRe - curIm * wIm;
                curIm = curRe * wIm + curIm * wRe;
                curRe = nextRe;
            }
        }
    }
}

/**
 * Average magnitude spectrum over the whole signal (Welch-style: Hann
 * windowed, 50% overlap).  Returns power per bin.
 */
export function averageSpectrum(mono, fftSize = 2048) {
    const hop = fftSize >> 1;
    const bins = fftSize >> 1;
    const acc = new Float64Array(bins);
    const window = new Float64Array(fftSize);
    for (let i = 0; i < fftSize; i++) {
        window[i] = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / fftSize);
    }

    const re = new Float64Array(fftSize);
    const im = new Float64Array(fftSize);
    let frames = 0;

    for (let start = 0; start + fftSize <= mono.length; start += hop) {
        for (let i = 0; i < fftSize; i++) {
            const v = mono[start + i];
            re[i] = Number.isFinite(v) ? v * window[i] : 0;
            im[i] = 0;
        }
        fft(re, im);
        for (let b = 0; b < bins; b++) {
            acc[b] += re[b] * re[b] + im[b] * im[b];
        }
        frames++;
    }

    if (frames > 0) {
        for (let b = 0; b < bins; b++) {
            acc[b] /= frames;
        }
    }
    return { power: acc, frames, fftSize };
}

/** Aggregate two average spectra into octave bands and report the delta. */
export function octaveBandDelta(specA, specB, sampleRate) {
    const bins = specA.power.length;
    const binHz = sampleRate / specA.fftSize;
    const edges = [20, 40, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480];
    const bands = [];

    for (let e = 0; e + 1 < edges.length; e++) {
        const lo = edges[e];
        const hi = Math.min(edges[e + 1], sampleRate / 2);
        if (lo >= hi) continue;
        let sumA = 0;
        let sumB = 0;
        let count = 0;
        for (let b = 1; b < bins; b++) {
            const f = b * binHz;
            if (f >= lo && f < hi) {
                sumA += specA.power[b];
                sumB += specB.power[b];
                count++;
            }
        }
        if (count === 0) continue;
        // Power ratio -> dB (10 log10), converted to an amplitude-equivalent
        // reading so it lines up with the other dB figures in the report.
        const aDb = sumA > 0 ? 10 * Math.log10(sumA / count) : MIN_DB;
        const bDb = sumB > 0 ? 10 * Math.log10(sumB / count) : MIN_DB;
        bands.push({
            loHz: lo,
            hiHz: hi,
            aDb: round(aDb, 2),
            bDb: round(bDb, 2),
            deltaDb: round(bDb - aDb, 2)
        });
    }
    return bands;
}

/**
 * Estimate how far b is shifted relative to a.
 *
 * Coarse pass over a decimated amplitude envelope, then a full-rate refine
 * around the coarse winner.  Positive lag means b is late.
 */
export function estimateLag(
    a,
    b,
    sampleRate,
    // 20 ms is ~7 render blocks: wide enough for any plausible engine
    // latency, narrow enough that the search cannot wander off and "find"
    // a spurious alignment half a second away.
    maxLagSeconds = 0.02,
    minConfidence = 0.3
) {
    const n = Math.min(a.length, b.length);
    if (n === 0) return 0;

    const maxLag = Math.min(Math.round(maxLagSeconds * sampleRate), n - 1);
    if (maxLag <= 0) return 0;

    const decim = 16;
    const envLen = Math.floor(n / decim);
    if (envLen < 4) return 0;

    const envA = new Float64Array(envLen);
    const envB = new Float64Array(envLen);
    for (let i = 0; i < envLen; i++) {
        let ma = 0;
        let mb = 0;
        const base = i * decim;
        for (let j = 0; j < decim; j++) {
            const va = Math.abs(a[base + j]);
            const vb = Math.abs(b[base + j]);
            if (va > ma) ma = va;
            if (vb > mb) mb = vb;
        }
        envA[i] = ma;
        envB[i] = mb;
    }

    const coarseMax = Math.max(1, Math.floor(maxLag / decim));
    const coarse = bestLag(envA, envB, coarseMax) * decim;

    // Refine at full rate over a short slice starting at the first audible
    // sample.  Keeping the window short matters: if the engines also drift
    // apart, a whole-file correlation would reject a constant offset that is
    // perfectly real at the start of the file.
    let onset = 0;
    while (onset < n && Math.abs(a[onset]) < 1e-4) onset++;
    if (onset >= n) return 0;
    // One second: long enough to be unambiguous, short enough that a rate
    // drift of a fraction of a cent has not yet walked out of phase.
    const windowEnd = Math.min(n, onset + Math.round(sampleRate * 1));

    const lo = coarse - decim;
    const hi = coarse + decim;
    let best = coarse;
    let bestScore = -Infinity;
    for (let lag = lo; lag <= hi; lag++) {
        const score = correlationAtLag(a, b, windowEnd, lag, onset);
        if (score > bestScore) {
            bestScore = score;
            best = lag;
        }
    }

    // When the two renders genuinely differ, the search happily latches onto
    // whatever lag maximizes noise — often the edge of the search range.
    // Reporting that as "the offset" is worse than reporting nothing, so a
    // weakly-supported alignment is discarded.
    if (!(bestScore > minConfidence)) {
        return 0;
    }
    return best;
}

/**
 * Track the alignment between a and b over time.
 *
 * A constant offset is engine latency; an offset that *drifts* means one
 * engine plays back at a slightly different rate, which destroys global
 * correlation while sounding essentially identical.  Distinguishing the two
 * is the whole point of this function.
 *
 * Returns one entry per analysis window: { tSec, lag, correlation, rmsA }.
 */
export function lagTrack(a, b, sampleRate, opts = {}) {
    const windowFrames = Math.round((opts.windowSeconds ?? 0.1) * sampleRate);
    const strideFrames = Math.round((opts.strideSeconds ?? 0.5) * sampleRate);
    const initialRange = Math.round((opts.maxLagSeconds ?? 0.05) * sampleRate);
    const trackRange = opts.trackRange ?? 48;
    const silenceFloor = opts.silenceFloor ?? 1e-5;
    // Only a confidently-matched window is allowed to seed the track; until
    // one shows up, keep searching the full range rather than following a
    // lag that was picked out of noise.
    const seedConfidence = opts.seedConfidence ?? 0.5;

    const n = Math.min(a.length, b.length);
    const track = [];
    let previous = null;

    for (let start = 0; start + windowFrames < n; start += strideFrames) {
        const level = rms(a, start, start + windowFrames);
        if (level < silenceFloor) continue;

        const center = previous ?? 0;
        const range = previous === null ? initialRange : trackRange;
        let best = center;
        let bestScore = -Infinity;
        for (let lag = center - range; lag <= center + range; lag++) {
            if (start + lag < 0 || start + windowFrames + lag > n) continue;
            let dot = 0;
            let na = 0;
            let nb = 0;
            for (let i = 0; i < windowFrames; i++) {
                const va = a[start + i];
                const vb = b[start + i + lag];
                dot += va * vb;
                na += va * va;
                nb += vb * vb;
            }
            if (na === 0 || nb === 0) continue;
            const score = dot / Math.sqrt(na * nb);
            if (score > bestScore) {
                bestScore = score;
                best = lag;
            }
        }
        if (bestScore === -Infinity) continue;
        if (previous !== null || bestScore >= seedConfidence) {
            previous = best;
        }
        track.push({
            tSec: start / sampleRate,
            lag: best,
            correlation: bestScore,
            rmsA: level
        });
    }
    return track;
}

/**
 * Least-squares slope of lag over time, expressed as a playback-rate error.
 *
 * A positive drift means the candidate falls progressively behind, i.e. it
 * plays slower and therefore flatter.
 */
export function lagDrift(track, sampleRate) {
    const usable = track.filter((t) => t.correlation > 0.5);
    if (usable.length < 3) {
        return { samplesPerSecond: 0, cents: 0, points: usable.length };
    }
    let sx = 0;
    let sy = 0;
    for (const t of usable) {
        sx += t.tSec;
        sy += t.lag;
    }
    const mx = sx / usable.length;
    const my = sy / usable.length;
    let num = 0;
    let den = 0;
    for (const t of usable) {
        num += (t.tSec - mx) * (t.lag - my);
        den += (t.tSec - mx) ** 2;
    }
    if (den === 0) {
        return { samplesPerSecond: 0, cents: 0, points: usable.length };
    }
    const samplesPerSecond = num / den;
    // lag(i) = i * (samplesPerSecond / sampleRate) + c, so the candidate's
    // timebase is stretched by (1 + slope); pitch moves the other way.
    const stretch = 1 + samplesPerSecond / sampleRate;
    const cents = stretch > 0 ? -1200 * Math.log2(stretch) : 0;
    return { samplesPerSecond, cents, points: usable.length };
}

/**
 * SNR measured with each window aligned by its own tracked lag: how different
 * the two renders are once any drift is factored out.
 */
export function trackedSnr(a, b, sampleRate, track, windowSeconds = 0.1) {
    const windowFrames = Math.round(windowSeconds * sampleRate);
    const n = Math.min(a.length, b.length);
    let signal = 0;
    let noise = 0;
    let count = 0;
    for (const t of track) {
        const start = Math.round(t.tSec * sampleRate);
        const lag = t.lag;
        if (start + lag < 0 || start + windowFrames + lag > n) continue;
        for (let i = 0; i < windowFrames; i++) {
            const va = a[start + i];
            const d = b[start + i + lag] - va;
            signal += va * va;
            noise += d * d;
            count++;
        }
    }
    if (count === 0 || noise === 0) return Infinity;
    return 20 * Math.log10(Math.sqrt(signal / count) / Math.sqrt(noise / count));
}

function bestLag(a, b, maxLag) {
    let best = 0;
    let bestScore = -Infinity;
    for (let lag = -maxLag; lag <= maxLag; lag++) {
        const score = correlationAtLag(a, b, Math.min(a.length, b.length), lag);
        if (score > bestScore) {
            bestScore = score;
            best = lag;
        }
    }
    return best;
}

function correlationAtLag(a, b, n, lag, start = 0) {
    let dot = 0;
    let na = 0;
    let nb = 0;
    const from = Math.max(start, -lag);
    const to = Math.min(n, b.length - lag);
    for (let i = from; i < to; i++) {
        const va = a[i];
        const vb = b[i + lag];
        dot += va * vb;
        na += va * va;
        nb += vb * vb;
    }
    if (na === 0 || nb === 0) return -Infinity;
    return dot / Math.sqrt(na * nb);
}

/** Least-squares gain that best maps a onto b, i.e. b[i + lag] ≈ gain * a[i]. */
export function bestFitGain(a, b, n, lag = 0) {
    let dot = 0;
    let energy = 0;
    const from = Math.max(0, -lag);
    const to = Math.min(n, n - lag);
    for (let i = from; i < to; i++) {
        dot += a[i] * b[i + lag];
        energy += a[i] * a[i];
    }
    return energy > 0 ? dot / energy : 1;
}

export function pearson(a, b, n) {
    let sa = 0;
    let sb = 0;
    for (let i = 0; i < n; i++) {
        sa += a[i];
        sb += b[i];
    }
    const ma = sa / n;
    const mb = sb / n;
    let cov = 0;
    let va = 0;
    let vb = 0;
    for (let i = 0; i < n; i++) {
        const da = a[i] - ma;
        const db = b[i] - mb;
        cov += da * db;
        va += da * da;
        vb += db * db;
    }
    if (va === 0 || vb === 0) return 0;
    return cov / Math.sqrt(va * vb);
}

/**
 * RMS of b[i + lag] - gain * a[i] over the overlapping region — the residual
 * left after explaining b as a shifted, rescaled copy of a.
 */
export function residualRms(a, b, n, lag = 0, gain = 1) {
    let sum = 0;
    let count = 0;
    const from = Math.max(0, -lag);
    const to = Math.min(n, n - lag);
    for (let i = from; i < to; i++) {
        const d = b[i + lag] - gain * a[i];
        sum += d * d;
        count++;
    }
    return count > 0 ? Math.sqrt(sum / count) : 0;
}

export function round(x, digits = 3) {
    if (!Number.isFinite(x)) return x;
    const f = 10 ** digits;
    return Math.round(x * f) / f;
}

/** Format a dB figure for humans; JSON.stringify turns Infinity into null. */
export function fmtDb(x) {
    if (x === Infinity) return "inf";
    if (x === -Infinity || x === MIN_DB) return "-inf";
    if (!Number.isFinite(x)) return "n/a";
    return x.toFixed(2);
}
