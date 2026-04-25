/**
 * system_exclusive.c
 * Core SysEx handler
 * Port of system_exclusive.ts.
 */

#define _USE_MATH_DEFINES
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

/* ── System Exclusive ────────────────────────────────────────────────────── */

extern void ss_sysex_handle_gm(SS_Processor *proc, const uint8_t *data, size_t len, double t, int channel_offset);
extern void ss_sysex_handle_gs(SS_Processor *proc, const uint8_t *data, size_t len, double t, int channel_offset);
extern void ss_sysex_handle_xg(SS_Processor *proc, const uint8_t *data, size_t len, double t, int channel_offset);

extern void ss_processor_event_emit(SS_Processor *proc, SS_SynthEventType type,
                                    int channel, int v1, int v2);

void ss_processor_sysex(SS_Processor *proc, const uint8_t *data, size_t len, double t) {
	if(!data || len < 1) return;

	int channel_offset = proc->port_select_channel_offset;

	uint8_t manufacturer = data[0];

	switch(manufacturer) {
		/* ── Universal Non-Realtime / Realtime ────────────────────────────── */
		case 0x7e: /* Non-realtime */
		case 0x7f: /* Realtime     */
			ss_sysex_handle_gm(proc, data, len, t, channel_offset);
			break;

		/* ── Roland GS ────────────────────────────────────────────────────── */
		case 0x41:
			ss_sysex_handle_gs(proc, data, len, t, channel_offset);
			break;

		/* ── Yamaha XG ────────────────────────────────────────────────────── */
		case 0x43:
			ss_sysex_handle_xg(proc, data, len, t, channel_offset);
			break;

		/* ── Port select (Falcosoft MIDI Player) ──────────────────────────── */
		/* https://www.vogons.org/viewtopic.php?p=1404746#p1404746 */
		case 0xf5: {
			if(len < 2) return;
			proc->port_select_channel_offset = (data[1] - 1) * 16;
			break;
		}
	}

	ss_processor_event_emit(proc, SS_EVENT_SYSEX, -1, 0, 0);
}
