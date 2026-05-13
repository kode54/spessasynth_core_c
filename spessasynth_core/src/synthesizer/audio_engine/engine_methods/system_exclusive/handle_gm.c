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

void ss_sysex_handle_gm(SS_Processor *proc, const uint8_t *syx, size_t len, double t, int channel_offset) {
	if(len < 4) return;
	/* syx[1] = device ID (0x7f = all), syx[2] = sub-ID1 */
	switch(syx[2]) {
		case 0x04: /* Device Control */
			if(len < 6) break;
			switch(syx[3]) {
				default:
					/* Unrecognized General MIDI real-time message */
					break;
				case 0x01: { /* Master Volume */
					const uint16_t vol = (uint16_t)((syx[5] << 7) | syx[4]);
					ss_processor_set_midi_volume(proc, (float)vol / 16384.0f);
					break;
				}
				case 0x02: {
					/* Master Balance / Pan
					 * MIDI spec page 62
					 */
					const uint16_t bal = (uint16_t)((syx[5] << 7) | syx[4]);
					proc->master_params.master_pan =
					((float)bal - 8192.0f) / 8192.0f;
					break;
				}
				case 0x03: { /* Fine Tuning */
					if(len < 7) break;
					const int raw = (int)(((syx[5] << 7) | syx[6]) - 8192);
					proc->master_params.master_tuning =
					(float)(raw / 81.92);
					break;
				}
				case 0x04: { /* Coarse Tuning */
					const int semitones = (int)syx[5] - 64;
					proc->master_params.master_pitch =
					(float)(semitones);
					break;
				}
			}
			break;

		case 0x05: {
			/* Global parameter control */
			if(len < 11) {
				/* Safety */
				break;
			}

			if(
				syx[4] != 0x01 || /* Slot Path Length */
				syx[5] != 0x01 || /* Parameter ID Width */
				syx[6] != 0x01 || /* Value Width */
				syx[7] != 0x01 /* Slot Path MSB */
			) {
				break;
			}
			switch(syx[8]) {
				default:
					/* Unknown global parameter */
					break;

				case 0x01: {
					/* Reverb */
					const uint8_t value = syx[10];
					/* Parameter */
					switch(syx[9]) {
						default:
							/* Unknown parameter */
							break;

						case 0x00: {
							/* Reverb type
							 * Match 8850 manual, page 231
							 * All match except for plate which is 8 in GM and 5 in GS
							 */
							const uint8_t macro = value == 0x08 ? 0x05 : value;
							ss_reverb_set_macro(proc->reverb, macro);
							break;
						}

						case 0x01: {
							/* Reverb time */
							ss_reverb_set_time(proc->reverb, value);
							break;
						}
					}
					break;
				}

				case 0x02: {
					/* Chorus */
					const uint8_t value = syx[10];
					/* Parameter */
					switch(syx[9]) {
						default:
							/* Unknown parameter */
							break;

						case 0x00: {
							/* Chorus type
							 * Match 8850 manual, page 231
							 * All match
							 */
							ss_chorus_set_macro(proc->chorus, value);
							break;
						}

						case 0x01: {
							/* Mod rate */
							ss_chorus_set_rate(proc->chorus, value);
							break;
						}

						case 0x02: {
							/* Mod depth */
							ss_chorus_set_depth(proc->chorus, value);
							break;
						}

						case 0x03: {
							/* Mod feedback */
							ss_chorus_set_feedback(proc->chorus, value);
							break;
						}

						case 0x04: {
							/* Mod send to reverb */
							ss_chorus_set_send_level_to_reverb(proc->chorus, value);
							break;
						}
					}
					break;
				}
			}
			break;
		}

		case 0x09: /* GM system */
			if(len < 4) break;
			switch(syx[3]) {
				default:
					/* Unrecognized General MIDI system message */
					break;
				case 0x01:
					proc->master_params.midi_system = SS_SYSTEM_GM;
					break;
				case 0x02:
					proc->master_params.midi_system = SS_SYSTEM_GS;
					break;
				case 0x03:
					proc->master_params.midi_system = SS_SYSTEM_GM2;
					break;
			}
			ss_processor_system_reset(proc);
			break;

		case 0x08: /* MIDI Tuning Standard */
		{
			if(len < 4) break;
			size_t idx = 4;
			switch(syx[3]) {
				case 0x01: { /* Bulk Tuning Dump */
					if(len < 384 + 4 + 16) break;
					int program = (int)syx[idx++];
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
						uint8_t b1 = syx[idx++];
						uint8_t b2 = syx[idx++];
						uint8_t b3 = syx[idx++];
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
					int program = (int)syx[idx++];
					int num_notes = (int)syx[idx++];
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
						uint8_t key = syx[idx++];
						uint8_t b1 = syx[idx++];
						uint8_t b2 = syx[idx++];
						uint8_t b3 = syx[idx++];
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
