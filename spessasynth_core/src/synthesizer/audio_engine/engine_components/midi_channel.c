/**
 * midi_channel.c
 * Per-MIDI-channel state and note management.
 * Port of midi_channel.ts.
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

extern SS_Voice *ss_voice_create(uint32_t sr,
                                 const SS_BasicPreset *preset,
                                 const SS_AudioSample *audio_sample,
                                 int midi_note, int velocity,
                                 double current_time, int target_key, int real_key,
                                 const int16_t *generators,
                                 const SS_Modulator *modulators, size_t mod_count,
                                 const SS_DynamicModulatorSystem *dms);
/*extern SS_Voice *ss_voice_copy(const SS_Voice *src, double current_time, int real_key);*/
extern void ss_voice_free(SS_Voice *v);
extern void ss_voice_release(SS_Voice *v, double current_time, double min_note_length);
extern void ss_voice_exclusive_release(SS_Voice *v, double current_time);
extern void ss_voice_compute_modulators(SS_Voice *v, const SS_MIDIChannel *ch, double time);
extern bool ss_voice_render(SS_Voice *v, const SS_MIDIChannel *ch,
                            double time_now,
                            float *ol, float *or_,
                            float *reverb,
                            float *chorus,
                            float *delay,
                            int sample_count,
                            SS_InterpolationType interp,
                            float vol_smoothing, float filter_smoothing, float pan_smoothing);
extern float ss_abs_cents_to_hz(int cents);
extern size_t ss_preset_get_synthesis_data(const SS_BasicPreset *preset,
                                           int midi_note, int velocity,
                                           SS_SynthesisData **out);
extern void ss_synthesis_data_free_array(SS_SynthesisData *data, size_t count);
extern bool ss_sample_decode(SS_BasicSample *s);

void ss_channel_set_custom_controller(SS_MIDIChannel *ch, SS_CustomController type, float val);
void ss_channel_set_tuning(SS_MIDIChannel *ch, float cents);
extern void ss_channel_exclusive_release(SS_MIDIChannel *ch, int note, double time);
extern void ss_channel_reset_drum_params(SS_MIDIChannel *ch);
extern void ss_channel_reset_controllers_to_defaults(SS_MIDIChannel *ch);
extern void ss_channel_compute_modulators(SS_MIDIChannel *ch, double time);

SS_MIDIChannel *ss_channel_new(int channel_number, struct SS_Processor *synth) {
	SS_MIDIChannel *ch = (SS_MIDIChannel *)calloc(1, sizeof(SS_MIDIChannel));
	if(!ch) return NULL;
	ch->channel_number = channel_number;
	ch->synth = synth;
	ch->drum_channel = (channel_number % 16 == 9);
	ch->poly_mode = true;
	ch->rx_channel = channel_number;
	ch->drum_map = 0;
	ch->cc1 = 0x10;
	ch->cc2 = 0x11;
	ss_channel_reset_drum_params(ch);
	ss_channel_reset_controllers_to_defaults(ch);
	return ch;
}

void ss_channel_free(SS_MIDIChannel *ch) {
	if(!ch) return;
	for(size_t i = 0; i < ch->voice_count; i++)
		ss_voice_free(ch->voices[i]);
	free(ch->voices);
	free(ch->sustained_voices);
	ss_dynamic_modulator_system_free(&ch->dms);
	free(ch);
}

/* ── Voice allocation ────────────────────────────────────────────────────── */

/* ── Pitch wheel ─────────────────────────────────────────────────────────── */

void ss_channel_pitch_wheel(SS_MIDIChannel *ch, int value, int midi_note, double time) {
	/* value: 0..16383, 8192 = center; midi_note == -1 for channel-wide pitch wheel */
	if(ch->locked_controllers[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL]) return;

	if(midi_note == -1) {
		/* Global pitch wheel: disable per-note mode */
		ch->per_note_pitch = false;
		ch->midi_controllers[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL] = (int16_t)value;
		ss_channel_compute_modulators(ch, time);
	} else {
		/* Per-note pitch wheel */
		if(!ch->per_note_pitch) {
			/* Entering per-note mode: seed all notes with the current global value */
			int16_t current = ch->midi_controllers[NON_CC_INDEX_OFFSET + SS_MODSRC_PITCH_WHEEL];
			for(int i = 0; i < 128; i++) ch->pitch_wheels[i] = current;
		}
		ch->per_note_pitch = true;
		ch->pitch_wheels[midi_note] = (int16_t)value;
		/* Recompute modulators only for active voices on this note */
		for(size_t i = 0; i < ch->voice_count; i++) {
			SS_Voice *v = ch->voices[i];
			if(v && v->is_active && v->midi_note == midi_note) {
				ss_voice_compute_modulators(v, ch, time);
			}
		}
	}
}
