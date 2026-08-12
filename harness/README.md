# Render comparison harness

Renders the same MIDI files through **spessasynth_core_c** (this repo) and
**spessasynth_core** (the upstream TypeScript library, expected at
`../../spessasynth_core`), then reports how the two audio outputs differ.

Both renderers are deliberately written as mirror images of each other —
same sample rate, same 128-sample render block, same total sample count,
sequencer tick before every block, no looping, no normalization, 32-bit float
output — so that anything the comparison reports comes from the synthesis
engines and not from the code driving them.

## Setup

```sh
./build.sh                 # builds the C library + harness/build/ss_render_c
node tools/make_probes.mjs # writes the single-mechanism probe MIDIs
```

`build.sh` configures its own build tree under `harness/build/core`, so the
repo's own `spessasynth_core/build` directory is left untouched.

The upstream repo needs its dependencies installed (`npm install` in
`../../spessasynth_core`) and `tsx` on `PATH`.

## Running

```sh
node run.mjs                          # upstream's generated test corpus
node run.mjs --gen                    # regenerate that corpus first
node run.mjs probes                   # the single-mechanism probes
node run.mjs path/to/file.mid         # specific files or directories
node run.mjs --sf /path/to/bank.sf2   # a different sound bank
```

Useful options: `--filter SUBSTR`, `--jobs N`, `--rate`, `--block`, `--tail`,
`--voice-cap`, `--auto-allocate`, `--no-effects`, `--fail-below DB`
(non-zero exit when any file scores below a given SNR — for CI),
`--timeline` (keep the full per-window timeline in the JSON reports).

Output lands in `harness/out`:

| Path | Contents |
| --- | --- |
| `c/`, `js/` | the two renders, 32-bit float WAV |
| `logs/` | each engine's stderr, side by side |
| `reports/<name>.json` | full metrics for one file |
| `summary.md`, `summary.json` | the cross-corpus table |

A single pair of WAVs can also be compared directly:

```sh
node compare.mjs reference.wav candidate.wav          # human readable
node compare.mjs reference.wav candidate.wav --json   # full report
```

The reference is upstream by convention, so every figure describes what the C
port does *relative to* upstream.

## Reading the report

Two independent implementations never match bit-for-bit, so the harness is
built to say *how* they differ rather than just *whether*. Three effects are
separated out, because each has a completely different cause:

- **Constant offset** (`lag`). A whole-block offset is engine latency — where
  each engine places events relative to the block it renders next. It is
  reported and then removed before anything else is measured; without that,
  a one-block offset alone drags every file to a negative SNR.
- **Rate drift** (`drift ¢`). If the best alignment *walks* over time, the two
  engines are playing back at slightly different rates. The slope is converted
  to a pitch error in cents. This is what makes global correlation collapse
  while the two renders still sound identical, so it is measured separately
  and tracked out.
- **Everything left over** (`SNR`, `window corr`). What remains once offset
  and drift are accounted for is a genuine synthesis difference. The
  per-window `first divergence` time says *when* it starts, which usually
  points straight at the event that causes it.

Supporting figures: `gain` (least-squares level difference — a large value
with an otherwise clean signal means only the volume differs), per-octave band
deltas (filter and effect differences), the worst 100 ms windows, and a count
of non-finite samples on either side.

Verdicts grade the drift-corrected SNR: `identical` (bit-exact),
`near-identical` (≥90 dB), `very-close` (≥60), `minor` (≥30), `moderate`
(≥12), `major` below that.

## Probe files

`tools/make_probes.mjs` writes minimal MIDI files that isolate one mechanism
each — one note, a release tail, a chromatic sweep, velocity steps, a pitch
bend, volume/pan sweeps, reverb/chorus sends, a drum hit, a chord. Upstream's
generated corpus exercises SysEx, effects and controllers all at once, which
makes it a poor place to start when the engines disagree; the probes let a
divergence be attributed rather than merely observed.
