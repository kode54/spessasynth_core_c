/* Exercises the seek paths, which the render corpus never touches: it only
 * ever seeks once, at load, before playback has moved the time base.
 *
 * Checks that after a seek the sequencer's own clock still agrees with the
 * processor's, that seeking by seconds and by tick land in the same place,
 * and that a seek past the end of the timeline is bounded. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spessasynth/midi/midi.h"
#include "spessasynth/sequencer/sequencer.h"
#include "spessasynth/soundbank/soundbank.h"
#include "spessasynth/synthesizer/synth.h"
#include "spessasynth/utils/file.h"

#define BLOCK 128
#define RATE 48000

static int failures = 0;

static void check(const char *what, bool ok, const char *detail) {
	printf("%-46s %s%s%s\n", what, ok ? "ok" : "FAIL",
	       detail && *detail ? "  " : "", detail ? detail : "");
	if(!ok) failures++;
}

/* seq->base_time + seq->current_time is the origin dispatched events are
 * stamped against.  The sequencer deliberately runs one render block behind
 * the processor, so after a tick+render cycle the two differ by exactly that
 * block and by nothing else; any other gap means the seek rebased wrongly. */
#define BLOCK_SECONDS ((double)BLOCK / (double)RATE)
static double clock_lag(const SS_Sequencer *seq, const SS_Processor *proc) {
	return proc->current_time - (seq->base_time + seq->current_time);
}

static void render(SS_Sequencer *seq, SS_Processor *proc, float *l, float *r,
                   int blocks) {
	for(int i = 0; i < blocks; i++) {
		ss_sequencer_tick(seq, BLOCK);
		ss_processor_render(proc, l, r, BLOCK);
	}
}

int main(int argc, char **argv) {
	if(argc < 3) {
		fprintf(stderr, "usage: seek_test <soundbank> <midi>\n");
		return 2;
	}

	SS_File *bf = ss_file_open_from_file(argv[1]);
	if(!bf) { fprintf(stderr, "bank open failed\n"); return 1; }
	SS_SoundBank *bank = ss_soundbank_load(bf);
	ss_file_close(bf);
	if(!bank) { fprintf(stderr, "bank parse failed\n"); return 1; }

	SS_File *mf = ss_file_open_from_file(argv[2]);
	if(!mf) { fprintf(stderr, "midi open failed\n"); return 1; }
	SS_MIDIFile *midi = ss_midi_load(mf, argv[2]);
	ss_file_close(mf);
	if(!midi) { fprintf(stderr, "midi parse failed\n"); return 1; }

	SS_ProcessorOptions opts = { .enable_effects = true,
		                         .voice_cap = 350,
		                         .interpolation = SS_INTERP_HERMITE,
		                         .preload_all_samples = false,
		                         .preload_instruments = true };
	SS_Processor *proc = ss_processor_create(RATE, &opts);
	ss_processor_load_soundbank(proc, bank, "main", 0, false);

	SS_Sequencer *seq = ss_sequencer_create(proc);
	ss_sequencer_set_loop_count(seq, 0);
	ss_sequencer_load_midi(seq, midi);
	ss_sequencer_play(seq);

	float *l = (float *)calloc(BLOCK, sizeof(float));
	float *r = (float *)calloc(BLOCK, sizeof(float));

	char detail[128];

	/* The lead-in skip already seeked at load. */
	snprintf(detail, sizeof(detail), "lag %.9f s", clock_lag(seq, proc));
	check("clock aligned after load-time seek", fabs(clock_lag(seq, proc)) < 1e-9,
	      detail);

	/* Play a while so base_time and current_time are both non-trivial, which
	 * is the case the load-time seek never produces. */
	render(seq, proc, l, r, RATE * 3 / BLOCK);
	snprintf(detail, sizeof(detail), "lag %.9f s, one block is %.9f s",
	         clock_lag(seq, proc), BLOCK_SECONDS);
	check("clock one block behind after playback",
	      fabs(clock_lag(seq, proc) - BLOCK_SECONDS) < 1e-9, detail);

	/* Seek mid-playback: this is where a song-relative timestamp would land
	 * base_time in the past. */
	const double target = midi->duration * 0.5;
	ss_sequencer_set_time(seq, target);
	const double seconds_landing = seq->current_time;
	snprintf(detail, sizeof(detail), "lag %.9f s", clock_lag(seq, proc));
	check("clock one block behind after mid-file seek",
	      fabs(clock_lag(seq, proc) - BLOCK_SECONDS) < 1e-9, detail);

	snprintf(detail, sizeof(detail), "asked %.3f s, landed %.3f s", target,
	         seq->current_time);
	check("seek by seconds lands near the target",
	      fabs(seq->current_time - target) < 0.5, detail);

	render(seq, proc, l, r, RATE / BLOCK);
	snprintf(detail, sizeof(detail), "lag %.9f s", clock_lag(seq, proc));
	check("clock one block behind after post-seek playback",
	      fabs(clock_lag(seq, proc) - BLOCK_SECONDS) < 1e-9, detail);

	/* Seeking by tick to the same place should land in the same place. */
	const size_t tick = ss_seconds_to_midi_tick(midi, target);
	ss_sequencer_set_tick(seq, tick);
	snprintf(detail, sizeof(detail), "by tick %.4f s vs by seconds %.4f s",
	         seq->current_time, seconds_landing);
	check("seek by tick agrees with seek by seconds",
	      fabs(seq->current_time - seconds_landing) < 0.05, detail);

	/* Past the end: must terminate and stay bounded, not run off the
	 * timeline. */
	ss_sequencer_set_time(seq, midi->duration * 10.0);
	snprintf(detail, sizeof(detail), "landed %.3f s, duration %.3f s",
	         seq->current_time, midi->duration);
	check("seek past the end is bounded",
	      seq->current_time <= midi->duration + 1.0, detail);

	printf("\n%s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
