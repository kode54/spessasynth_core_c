/**
 * program_change.c
 * Per-MIDI-channel program change handler.
 * Port of program_change.ts.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/midi_enums.h>
#include <spessasynth_core/synth.h>
#else
#include "spessasynth/midi/midi_enums.h"
#include "spessasynth/synthesizer/synth.h"
#endif

/* ── Program change ──────────────────────────────────────────────────────── */

extern void ss_channel_reset_drum_params(SS_MIDIChannel *ch);
extern void ss_preset_precache(SS_BasicPreset *p);

void ss_channel_program_change(SS_MIDIChannel *ch, int program) {
	if(ch->lock_preset) return;
	ch->program = (uint8_t)(program & 0x7F);
	/* Look up preset in the soundbank(s) via processor */
	struct SS_Processor *proc = ch->synth;
	if(!proc) return;
	SS_BasicPreset *p = ss_processor_resolve_preset(proc, ch->channel_number,
	                                                ch->program, ch->bank_msb, ch->bank_lsb,
	                                                ch->drum_channel);
	if(p) {
		ch->preset = p;
		bool is_drum_preset = p->is_gm_gs_drum || p->is_xg_drum;
		if(ch->drum_channel != is_drum_preset) {
			ch->drum_channel = is_drum_preset;
		}
		ss_channel_reset_drum_params(ch);
		if(ch->synth && ch->synth->options.preload_instruments) ss_preset_precache(p);
		return;
	}
}
