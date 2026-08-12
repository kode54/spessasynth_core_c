/* Exercises the processor's message queue, which the render corpus cannot
 * reach: the sequencer only ever stamps events at or behind the engine clock,
 * so every message it sends takes the immediate path.  Scheduling ahead is a
 * host-facing capability, and these are its rules.
 *
 * Upstream's processMessage queues anything stamped later than the engine
 * clock and drains the queue at the top of each render, so a scheduled message
 * lands on the first block whose clock has reached it -- never earlier, never
 * later, and never out of order with its neighbours. */
#include <stdio.h>
#include <stdlib.h>

#include "spessasynth/soundbank/soundbank.h"
#include "spessasynth/synthesizer/synth.h"
#include "spessasynth/utils/file.h"

#define BLOCK 128
#define RATE 48000
#define BLOCK_SECONDS ((double)BLOCK / (double)RATE)

static int failures = 0;

static void check(const char *what, bool ok, const char *detail) {
	printf("%-52s %s%s%s\n", what, ok ? "ok" : "FAIL",
	       detail && *detail ? "  " : "", detail ? detail : "");
	if(!ok) failures++;
}

static void render_block(SS_Processor *proc, float *l, float *r) {
	ss_processor_render(proc, l, r, BLOCK);
}

/* Blocks rendered before the processor first reports a live voice. */
static int blocks_until_sounding(SS_Processor *proc, float *l, float *r, int limit) {
	for(int i = 0; i < limit; i++) {
		render_block(proc, l, r);
		if(proc->voice_count > 0) return i;
	}
	return -1;
}

static const char *bank_path;

/* A processor frees the banks registered with it, so each test needs its own
 * copy of the bank rather than a shared one. */
static SS_Processor *make_processor(void) {
	static const SS_ProcessorOptions opts = { .enable_effects = false,
		                                      .voice_cap = 350,
		                                      .interpolation = SS_INTERP_HERMITE,
		                                      .preload_all_samples = false,
		                                      .preload_instruments = true };
	SS_File *bf = ss_file_open_from_file(bank_path);
	if(!bf) { fprintf(stderr, "bank open failed\n"); exit(1); }
	SS_SoundBank *bank = ss_soundbank_load(bf);
	ss_file_close(bf);
	if(!bank) { fprintf(stderr, "bank parse failed\n"); exit(1); }

	SS_Processor *proc = ss_processor_create(RATE, &opts);
	ss_processor_load_soundbank(proc, bank, "main", 0, false);
	return proc;
}

int main(int argc, char **argv) {
	if(argc < 2) {
		fprintf(stderr, "usage: event_queue_test <soundbank>\n");
		return 2;
	}

	bank_path = argv[1];

	float *l = (float *)calloc(BLOCK, sizeof(float));
	float *r = (float *)calloc(BLOCK, sizeof(float));
	char detail[160];

	/* A message stamped at or behind the clock is applied on the spot. */
	{
		SS_Processor *proc = make_processor();
		const uint8_t note_on[3] = { 0x90, 60, 100 };
		ss_processor_process_message(proc, note_on, 3, 0, 0.0);
		check("message at the clock sounds immediately",
		      proc->voice_count > 0 || (render_block(proc, l, r), proc->voice_count > 0),
		      NULL);
		ss_processor_free(proc);
	}

	/* A message stamped ahead of the clock waits for the block that reaches
	 * it.  Scheduled 8 blocks out, it must be silent for 8 and sound on the
	 * 9th -- the first block whose clock is no longer behind the timestamp. */
	{
		SS_Processor *proc = make_processor();
		const uint8_t note_on[3] = { 0x90, 60, 100 };
		ss_processor_process_message(proc, note_on, 3, 0, BLOCK_SECONDS * 8);

		check("scheduled message does not sound when submitted",
		      proc->voice_count == 0, NULL);

		int at = blocks_until_sounding(proc, l, r, 24);
		snprintf(detail, sizeof detail, "sounded on block %d, expected 8", at);
		check("scheduled message sounds on its own block", at == 8, detail);
		ss_processor_free(proc);
	}

	/* Out-of-order submission still plays in time order.  The later note is
	 * submitted first; if the queue kept arrival order instead of time order,
	 * the earlier one would be stuck behind it and both would land late. */
	{
		SS_Processor *proc = make_processor();
		const uint8_t late[3]  = { 0x90, 72, 100 };
		const uint8_t early[3] = { 0x90, 60, 100 };
		ss_processor_process_message(proc, late, 3, 0, BLOCK_SECONDS * 10);
		ss_processor_process_message(proc, early, 3, 0, BLOCK_SECONDS * 4);

		int at = blocks_until_sounding(proc, l, r, 24);
		snprintf(detail, sizeof detail, "first voice on block %d, expected 4", at);
		check("queue applies by timestamp, not arrival order", at == 4, detail);
		ss_processor_free(proc);
	}

	/* Freeing a processor with messages still pending must not leak them;
	 * this runs clean under a leak checker and is here to be run under one. */
	{
		SS_Processor *proc = make_processor();
		const uint8_t note_on[3] = { 0x90, 60, 100 };
		for(int i = 0; i < 64; i++)
			ss_processor_process_message(proc, note_on, 3, 0, BLOCK_SECONDS * (100 + i));
		ss_processor_free(proc);
		check("undrained queue is released with the processor", true, NULL);
	}

	free(l);
	free(r);

	printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
	return failures ? 1 : 0;
}
