/*
 * ss_render_c — offline WAV renderer for the C port.
 *
 * Deliberately mirrors harness/render_js.ts step for step so that any
 * difference in the output files is attributable to the synthesis core and
 * not to the driver around it:
 *
 *   - identical sample rate, block size and total sample count
 *   - sequencer tick before every render block
 *   - no looping, no EMIDI filtering, no normalization
 *   - 32-bit float WAV output (no quantization on the comparison path)
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spessasynth/midi/midi.h"
#include "spessasynth/sequencer/sequencer.h"
#include "spessasynth/soundbank/soundbank.h"
#include "spessasynth/synthesizer/synth.h"
#include "spessasynth/utils/file.h"

/* Defaults chosen to match spessasynth_core's own defaults, so that both
 * renderers start from the same synth state. */
#define DEF_RATE 48000
#define DEF_TAIL 2.0
#define DEF_BLOCK 128
#define DEF_VOICE_CAP 350

typedef struct {
	uint32_t rate;
	double tail;
	uint32_t block;
	uint32_t voice_cap;
	bool auto_allocate;
	bool effects;
	int loop_count;
	bool emidi_filter;
	bool preload_samples;
	bool no_skip;
	SS_InterpolationType interp;
	const char *bank_path;
	const char *midi_path;
	const char *out_path;
} Options;

static void usage(const char *argv0) {
	fprintf(stderr,
	        "usage: %s [options] <soundbank> <midi> <out.wav>\n"
	        "\n"
	        "  --rate N            sample rate (default %d)\n"
	        "  --tail S            seconds of tail after the song (default %.1f)\n"
	        "  --block N           render block size (default %d)\n"
	        "  --voice-cap N       voice cap (default %d)\n"
	        "  --auto-allocate     uncapped voice allocation (default off)\n"
	        "  --no-effects        disable reverb/chorus/delay\n"
	        "  --interp T          linear|nearest|hermite|sinc (default hermite)\n"
	        "  --loop-count N      sequencer loop count (default 0, i.e. no loop)\n"
	        "  --emidi-filter      apply the C port's EMIDI non-GM filtering\n"
	        "  --preload           preload every sample before rendering\n",
	        argv0, DEF_RATE, DEF_TAIL, DEF_BLOCK, DEF_VOICE_CAP);
}

static bool parse_interp(const char *s, SS_InterpolationType *out) {
	if(strcmp(s, "linear") == 0) { *out = SS_INTERP_LINEAR; return true; }
	if(strcmp(s, "nearest") == 0) { *out = SS_INTERP_NEAREST; return true; }
	if(strcmp(s, "hermite") == 0) { *out = SS_INTERP_HERMITE; return true; }
	if(strcmp(s, "sinc") == 0) { *out = SS_INTERP_SINC; return true; }
	return false;
}

/* ── 32-bit float WAV ───────────────────────────────────────────────────── */

static void put_u32(FILE *f, uint32_t v) {
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
	fwrite(b, 1, 4, f);
}

static void put_u16(FILE *f, uint16_t v) {
	uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
	fwrite(b, 1, 2, f);
}

static bool write_wav_f32(const char *path, const float *left, const float *right,
                          uint32_t frames, uint32_t rate) {
	FILE *f = fopen(path, "wb");
	if(!f) {
		fprintf(stderr, "Could not open '%s' for writing\n", path);
		return false;
	}
	const uint32_t channels = 2;
	const uint32_t data_bytes = frames * channels * 4;

	fwrite("RIFF", 1, 4, f);
	put_u32(f, 4 + (8 + 16) + (8 + data_bytes));
	fwrite("WAVE", 1, 4, f);

	fwrite("fmt ", 1, 4, f);
	put_u32(f, 16);
	put_u16(f, 3); /* IEEE float */
	put_u16(f, (uint16_t)channels);
	put_u32(f, rate);
	put_u32(f, rate * channels * 4);
	put_u16(f, (uint16_t)(channels * 4));
	put_u16(f, 32);

	fwrite("data", 1, 4, f);
	put_u32(f, data_bytes);

	/* Interleave in chunks rather than one fwrite per sample. */
	enum { CHUNK = 4096 };
	float buf[CHUNK * 2];
	for(uint32_t i = 0; i < frames;) {
		uint32_t n = frames - i;
		if(n > CHUNK) n = CHUNK;
		for(uint32_t j = 0; j < n; j++) {
			buf[j * 2] = left[i + j];
			buf[j * 2 + 1] = right[i + j];
		}
		if(fwrite(buf, sizeof(float), n * 2, f) != n * 2) {
			fclose(f);
			fprintf(stderr, "Short write to '%s'\n", path);
			return false;
		}
		i += n;
	}
	fclose(f);
	return true;
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
	Options o = {
		.rate = DEF_RATE,
		.tail = DEF_TAIL,
		.block = DEF_BLOCK,
		.voice_cap = DEF_VOICE_CAP,
		.auto_allocate = false,
		.effects = true,
		.loop_count = 0,
		.emidi_filter = false,
		.preload_samples = false,
		.no_skip = false,
		.interp = SS_INTERP_HERMITE
	};

	const char *positional[3] = { NULL, NULL, NULL };
	size_t positional_count = 0;

	for(int i = 1; i < argc; i++) {
		const char *a = argv[i];
		bool needs_value = strcmp(a, "--rate") == 0 || strcmp(a, "--tail") == 0 ||
		                   strcmp(a, "--block") == 0 || strcmp(a, "--voice-cap") == 0 ||
		                   strcmp(a, "--interp") == 0 || strcmp(a, "--loop-count") == 0;
		if(needs_value && i + 1 >= argc) {
			fprintf(stderr, "%s requires a value\n", a);
			return 2;
		}

		if(strcmp(a, "--rate") == 0) {
			o.rate = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else if(strcmp(a, "--tail") == 0) {
			o.tail = strtod(argv[++i], NULL);
		} else if(strcmp(a, "--block") == 0) {
			o.block = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else if(strcmp(a, "--voice-cap") == 0) {
			o.voice_cap = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else if(strcmp(a, "--loop-count") == 0) {
			o.loop_count = (int)strtol(argv[++i], NULL, 10);
		} else if(strcmp(a, "--interp") == 0) {
			if(!parse_interp(argv[++i], &o.interp)) {
				fprintf(stderr, "Unknown interpolation type '%s'\n", argv[i]);
				return 2;
			}
		} else if(strcmp(a, "--auto-allocate") == 0) {
			o.auto_allocate = true;
		} else if(strcmp(a, "--no-effects") == 0) {
			o.effects = false;
		} else if(strcmp(a, "--emidi-filter") == 0) {
			o.emidi_filter = true;
		} else if(strcmp(a, "--no-skip") == 0) {
			o.no_skip = true;
		} else if(strcmp(a, "--preload") == 0) {
			o.preload_samples = true;
		} else if(strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else if(a[0] == '-' && a[1] != '\0') {
			fprintf(stderr, "Unknown option '%s'\n", a);
			return 2;
		} else if(positional_count < 3) {
			positional[positional_count++] = a;
		} else {
			fprintf(stderr, "Too many arguments\n");
			return 2;
		}
	}

	if(positional_count != 3 || o.rate == 0 || o.block == 0) {
		usage(argv[0]);
		return 2;
	}
	o.bank_path = positional[0];
	o.midi_path = positional[1];
	o.out_path = positional[2];

	/* Sound bank */
	SS_File *bank_file = ss_file_open_from_file(o.bank_path);
	if(!bank_file) {
		fprintf(stderr, "Could not open sound bank '%s'\n", o.bank_path);
		return 1;
	}
	SS_SoundBank *bank = ss_soundbank_load(bank_file);
	ss_file_close(bank_file);
	if(!bank) {
		fprintf(stderr, "Could not parse sound bank '%s'\n", o.bank_path);
		return 1;
	}

	/* MIDI */
	SS_File *midi_file = ss_file_open_from_file(o.midi_path);
	if(!midi_file) {
		fprintf(stderr, "Could not open MIDI '%s'\n", o.midi_path);
		return 1;
	}
	SS_MIDIFile *midi = ss_midi_load(midi_file, o.midi_path);
	ss_file_close(midi_file);
	if(!midi) {
		fprintf(stderr, "Could not parse MIDI '%s'\n", o.midi_path);
		return 1;
	}
	if(o.emidi_filter && ss_midi_has_emidi(midi)) {
		ss_midi_remove_emidi_non_gm(midi);
	}

	/* Synth */
	SS_ProcessorOptions popts = {
		.enable_effects = o.effects,
		.voice_cap = o.voice_cap,
		.interpolation = o.interp,
		.preload_all_samples = o.preload_samples,
		.preload_instruments = true
	};
	SS_Processor *proc = ss_processor_create(o.rate, &popts);
	if(!proc) {
		fprintf(stderr, "Could not create the synthesizer\n");
		return 1;
	}
	ss_processor_set_system_parameter(proc, SS_GLOBAL_SYS_EVENTS_ENABLED, 0);
	ss_processor_set_system_parameter(proc, SS_GLOBAL_SYS_AUTO_ALLOCATE_VOICES,
	                                  o.auto_allocate ? 1 : 0);

	if(!ss_processor_load_soundbank(proc, bank, "main", 0, false)) {
		fprintf(stderr, "Could not register the sound bank\n");
		return 1;
	}

	SS_Sequencer *seq = ss_sequencer_create(proc);
	if(!seq) {
		fprintf(stderr, "Out of memory\n");
		return 1;
	}
	/* The C sequencer defaults to one extra playthrough of the loop body;
	 * the JS sequencer defaults to none.  Match the JS behavior. */
	ss_sequencer_set_loop_count(seq, o.loop_count);
	if(o.no_skip) ss_sequencer_set_skip_to_first_note_on(seq, false);
	if(!ss_sequencer_load_midi(seq, midi)) {
		fprintf(stderr, "Could not load the MIDI into the sequencer\n");
		return 1;
	}
	ss_sequencer_play(seq);

	const double duration = midi->duration;
	const uint32_t frames = (uint32_t)ceil((double)o.rate * (duration + o.tail));
	if(frames == 0) {
		fprintf(stderr, "Nothing to render (duration %.3f s)\n", duration);
		return 1;
	}

	float *left = (float *)calloc(frames, sizeof(float));
	float *right = (float *)calloc(frames, sizeof(float));
	if(!left || !right) {
		fprintf(stderr, "Out of memory allocating %u frames\n", frames);
		return 1;
	}

	fprintf(stderr, "[c] %s: duration %.3f s, rendering %u frames at %u Hz\n",
	        o.midi_path, duration, frames, o.rate);

	for(uint32_t filled = 0; filled < frames;) {
		uint32_t n = frames - filled;
		if(n > o.block) n = o.block;
		ss_sequencer_tick(seq, n);
		ss_processor_render(proc, left + filled, right + filled, n);
		filled += n;
	}

	if(!write_wav_f32(o.out_path, left, right, frames, o.rate)) {
		return 1;
	}
	fprintf(stderr, "[c] wrote %s\n", o.out_path);

	/* Leave teardown to the OS: nothing here outlives the process, and the
	 * harness cares about determinism of the samples, not of the heap. */
	return 0;
}
