/* Verifies callback-mode lead-in: setup messages must arrive spread over
 * real time, and the lead-in flag must clear exactly when the first note
 * is dispatched. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spessasynth/midi/midi.h"
#include "spessasynth/sequencer/sequencer.h"
#include "spessasynth/utils/file.h"

#define BLOCK 128
#define RATE 48000

static int g_block = 0;
static int g_msgs = 0;
static int g_first_note_block = -1;
static int g_distinct_blocks = 0;
static int g_last_msg_block = -1;
static int g_lead_in_end_block = -1;

static void on_midi(void *ctx, const uint8_t *data, size_t len, double t) {
	(void)ctx; (void)t;
	g_msgs++;
	if(g_block != g_last_msg_block) {
		g_distinct_blocks++;
		g_last_msg_block = g_block;
	}
	if(len >= 3 && (data[0] & 0xF0) == 0x90 && data[2] > 0 &&
	   g_first_note_block < 0) {
		g_first_note_block = g_block;
	}
}

int main(int argc, char **argv) {
	if(argc < 2) { fprintf(stderr, "usage: cbtest <midi>\n"); return 2; }

	SS_File *f = ss_file_open_from_file(argv[1]);
	if(!f) { fprintf(stderr, "open failed\n"); return 1; }
	SS_MIDIFile *midi = ss_midi_load(f, argv[1]);
	ss_file_close(f);
	if(!midi) { fprintf(stderr, "parse failed\n"); return 1; }

	SS_SequencerCallbacks cb;
	memset(&cb, 0, sizeof(cb));
	cb.midi_command = on_midi;
	cb.sample_rate = RATE;

	SS_Sequencer *seq = ss_sequencer_create_callbacks(&cb);
	ss_sequencer_set_loop_count(seq, 0);
	if(!ss_sequencer_load_midi(seq, midi)) { fprintf(stderr, "load failed\n"); return 1; }

	printf("first_note_on tick=%zu (%.4f s)  lead_in at load=%s\n",
	       (size_t)midi->first_note_on,
	       ss_midi_ticks_to_seconds(midi, midi->first_note_on),
	       ss_sequencer_is_lead_in(seq) ? "true" : "false");

	ss_sequencer_play(seq);

	int discarded = 0;
	for(g_block = 0; g_block < RATE * 6 / BLOCK; g_block++) {
		ss_sequencer_tick(seq, BLOCK);
		bool discard = ss_sequencer_is_lead_in(seq);
		if(discard) discarded++;
		else if(g_lead_in_end_block < 0) g_lead_in_end_block = g_block;
		if(ss_sequencer_is_finished(seq)) break;
	}

	printf("messages=%d delivered across %d distinct blocks\n", g_msgs, g_distinct_blocks);
	printf("first note dispatched in block %d (%.4f s)\n",
	       g_first_note_block, g_first_note_block * (double)BLOCK / RATE);
	printf("lead-in cleared at block %d (%.4f s), %d blocks discarded\n",
	       g_lead_in_end_block, g_lead_in_end_block * (double)BLOCK / RATE, discarded);

	int ok = 1;
	if(g_distinct_blocks < 2) {
		printf("FAIL: lead-in was delivered as a single burst\n");
		ok = 0;
	}
	if(g_lead_in_end_block != g_first_note_block) {
		printf("FAIL: lead-in cleared at block %d but first note was block %d\n",
		       g_lead_in_end_block, g_first_note_block);
		ok = 0;
	}
	printf(ok ? "PASS\n" : "FAIL\n");
	return ok ? 0 : 1;
}
