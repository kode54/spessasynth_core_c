/**
 * handle_gm.c
 * General MIDI SysEx handler
 * Port of handle_gm.ts.
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

/* ── System Exclusive: General MIDI ──────────────────────────────────────── */

void ss_processor_set_midi_volume(SS_Processor *proc, float volume);

void ss_sysex_handle_gm(SS_Processor *proc, const uint8_t *data, size_t len, double t, int channel_offset) {
	if(len < 4) return;
	/* data[1] = device ID (0x7f = all), data[2] = sub-ID1 */
	switch(data[2]) {
		case 0x04: /* Device Control */
			if(len < 6) break;
			switch(data[3]) {
				case 0x01: { /* Master Volume */
					uint16_t vol = (uint16_t)((data[5] << 7) | data[4]);
					ss_processor_set_midi_volume(proc, (float)vol / 16384.0f);
					break;
				}
				case 0x02: { /* Master Balance / Pan */
					uint16_t bal = (uint16_t)((data[5] << 7) | data[4]);
					proc->master_params.master_pan =
					((float)bal - 8192.0f) / 8192.0f;
					break;
				}
				case 0x03: { /* Fine Tuning */
					if(len < 7) break;
					int raw = (int)(((data[5] << 7) | data[6]) - 8192);
					proc->master_params.master_tuning =
					(float)(raw / 81.92);
					break;
				}
				case 0x04: { /* Coarse Tuning */
					int semitones = (int)data[5] - 64;
					proc->master_params.master_tuning =
					(float)(semitones * 100);
					break;
				}
			}
			break;

		case 0x09: /* GM system */
			if(len < 4) break;
			if(data[3] == 0x01) {
				proc->master_params.midi_system = SS_SYSTEM_GM;
			} else if(data[3] == 0x03) {
				proc->master_params.midi_system = SS_SYSTEM_GM;
			} else {
				proc->master_params.midi_system = SS_SYSTEM_GS;
			}
			ss_processor_system_reset(proc);
			break;

		case 0x08: /* MIDI Tuning Standard */
		{
			if(len < 4) break;
			size_t idx = 4;
			switch(data[3]) {
				case 0x01: { /* Bulk Tuning Dump */
					if(len < 384 + 4 + 16) break;
					int program = (int)data[idx++];
					/* skip 16-byte name */
					idx += 16;
					/* Ensure tuning grid exists */
					if(!proc->master_params.tunings) {
						proc->master_params.tunings =
						(SS_TuningEntry **)calloc(128, sizeof(SS_TuningEntry *));
						if(!proc->master_params.tunings) return;
					}
					if(!proc->master_params.tunings[program]) {
						proc->master_params.tunings[program] =
						(SS_TuningEntry *)malloc(128 * sizeof(SS_TuningEntry));
						if(!proc->master_params.tunings[program]) return;
						for(int n = 0; n < 128; n++) {
							proc->master_params.tunings[program][n].midi_note = -1;
							proc->master_params.tunings[program][n].cent_tuning = 0;
						}
					}
					for(int n = 0; n < 128 && idx + 2 < len; n++) {
						uint8_t b1 = data[idx++];
						uint8_t b2 = data[idx++];
						uint8_t b3 = data[idx++];
						if(b1 == 0x7f && b2 == 0x7f && b3 == 0x7f) {
							proc->master_params.tunings[program][n].midi_note = n;
							proc->master_params.tunings[program][n].cent_tuning = 0.0f;
						} else {
							int fraction = (b2 << 7) | b3;
							proc->master_params.tunings[program][n].midi_note = (int)b1;
							proc->master_params.tunings[program][n].cent_tuning =
							(float)(fraction * 0.0061);
						}
					}
					break;
				}
				case 0x02: { /* Single Note Tuning Change */
					if(len < 4 + 6) break;
					int program = (int)data[idx++];
					int num_notes = (int)data[idx++];
					if(!proc->master_params.tunings) {
						proc->master_params.tunings =
						(SS_TuningEntry **)calloc(128, sizeof(SS_TuningEntry *));
						if(!proc->master_params.tunings) return;
					}
					if(!proc->master_params.tunings[program]) {
						proc->master_params.tunings[program] =
						(SS_TuningEntry *)malloc(128 * sizeof(SS_TuningEntry));
						if(!proc->master_params.tunings[program]) return;
						for(int n = 0; n < 128; n++) {
							proc->master_params.tunings[program][n].midi_note = -1;
							proc->master_params.tunings[program][n].cent_tuning = 0;
						}
					}
					for(int ni = 0; ni < num_notes && idx + 3 <= len; ni++) {
						uint8_t key = data[idx++];
						uint8_t b1 = data[idx++];
						uint8_t b2 = data[idx++];
						uint8_t b3 = data[idx++];
						if(b1 == 0x7f && b2 == 0x7f && b3 == 0x7f) continue;
						int fraction = (b2 << 7) | b3;
						proc->master_params.tunings[program][key].midi_note = (int)b1;
						proc->master_params.tunings[program][key].cent_tuning =
						(float)(fraction * 0.0061);
					}
					break;
				}
			}
			break;
		}
	}
}
