/*
 * chan_probe — reports what preset every MIDI channel is actually using while
 * a file plays, so a "wrong instrument" report can be attributed to preset
 * resolution rather than guessed at from the audio.
 *
 * Prints one line whenever a channel's program, bank, drum flag or resolved
 * preset changes, plus a per-channel summary of note-ons at the end.
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

#define RATE 48000
#define BLOCK 128

typedef struct {
	uint8_t program, bank_msb, bank_lsb;
	bool drum;
	const SS_BasicPreset *preset;
	bool valid;
} Snapshot;

int main(int argc, char *argv[]) {
	if(argc < 3) {
		fprintf(stderr, "usage: %s <soundbank> <midi> [--no-skip]\n", argv[0]);
		return 2;
	}
	bool no_skip = false;
	for(int i = 3; i < argc; i++)
		if(strcmp(argv[i], "--no-skip") == 0) no_skip = true;

	SS_File *bf = ss_file_open_from_file(argv[1]);
	if(!bf) { fprintf(stderr, "cannot open bank\n"); return 1; }
	SS_SoundBank *bank = ss_soundbank_load(bf);
	ss_file_close(bf);
	if(!bank) { fprintf(stderr, "cannot parse bank\n"); return 1; }

	SS_File *mf = ss_file_open_from_file(argv[2]);
	if(!mf) { fprintf(stderr, "cannot open midi\n"); return 1; }
	SS_MIDIFile *midi = ss_midi_load(mf, argv[2]);
	ss_file_close(mf);
	if(!midi) { fprintf(stderr, "cannot parse midi\n"); return 1; }

	printf("bank \"%s\": %zu presets, is_xg_bank=%d\n", bank->name, bank->preset_count,
	       bank->is_xg_bank ? 1 : 0);

	SS_ProcessorOptions popts = {
		.enable_effects = true,
		.voice_cap = 350,
		.interpolation = SS_INTERP_HERMITE,
		.preload_all_samples = false,
		.preload_instruments = true
	};
	SS_Processor *proc = ss_processor_create(RATE, &popts);
	ss_processor_set_system_parameter(proc, SS_GLOBAL_SYS_EVENTS_ENABLED, 0);
	if(!ss_processor_load_soundbank(proc, bank, "main", 0, false)) {
		fprintf(stderr, "cannot register bank\n");
		return 1;
	}

	SS_Sequencer *seq = ss_sequencer_create(proc);
	ss_sequencer_set_loop_count(seq, 0);
	if(no_skip) ss_sequencer_set_skip_to_first_note_on(seq, false);
	if(!ss_sequencer_load_midi(seq, midi)) { fprintf(stderr, "cannot load midi\n"); return 1; }
	ss_sequencer_play(seq);

	Snapshot snap[16];
	memset(snap, 0, sizeof(snap));
	unsigned long long note_ons[16];
	memset(note_ons, 0, sizeof(note_ons));
	bool prev_playing[16][128];
	memset(prev_playing, 0, sizeof(prev_playing));

	const double duration = midi->duration;
	const uint32_t frames = (uint32_t)ceil((double)RATE * (duration + 1.0));
	float *l = (float *)calloc(BLOCK, sizeof(float));
	float *r = (float *)calloc(BLOCK, sizeof(float));

	for(uint32_t filled = 0; filled < frames; filled += BLOCK) {
		uint32_t n = frames - filled;
		if(n > BLOCK) n = BLOCK;
		ss_sequencer_tick(seq, n);
		ss_processor_render(proc, l, r, n);
		double t = (double)filled / RATE;
		for(int c = 0; c < 16; c++) {
			SS_MIDIChannel *ch = proc->midi_channels[c];
			if(!ch) continue;
			for(int k = 0; k < 128; k++) {
				if(ch->playing_notes[k] && !prev_playing[c][k]) note_ons[c]++;
				prev_playing[c][k] = ch->playing_notes[k];
			}
			Snapshot cur = { ch->program, ch->bank_msb, ch->bank_lsb,
				             ch->drum_channel, ch->preset, true };
			Snapshot *old = &snap[c];
			if(old->valid && old->program == cur.program && old->bank_msb == cur.bank_msb &&
			   old->bank_lsb == cur.bank_lsb && old->drum == cur.drum && old->preset == cur.preset)
				continue;
			*old = cur;
			printf("t=%7.3f ch%-2d prog=%-3d bank=%d:%-3d drum=%d preset=%s",
			       t, c + 1, cur.program, cur.bank_msb, cur.bank_lsb, cur.drum ? 1 : 0,
			       cur.preset ? cur.preset->name : "(none)");
			if(cur.preset)
				printf("  [%d:%d prog %d gs_drum=%d]", cur.preset->bank_msb,
				       cur.preset->bank_lsb, cur.preset->program,
				       cur.preset->is_gm_gs_drum ? 1 : 0);
			printf("\n");
		}
	}

	printf("\n-- note-ons per channel --\n");
	for(int c = 0; c < 16; c++)
		if(note_ons[c]) printf("ch%-2d %llu\n", c + 1, note_ons[c]);
	return 0;
}
