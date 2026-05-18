/**
 * parameters.c
 * System and MIDI parameters, on a per-channel and a global (system) level.
 * Port of:
 *   synthesizer/audio_engine/parameters/{system,midi}.ts
 *   synthesizer/audio_engine/channel/parameters/{system,midi}.ts
 *   the updateInternalParams() method of midi_channel.ts
 */

#include <math.h>
#include <stddef.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/synth.h>
#else
#include "spessasynth/synthesizer/synth.h"
#endif

/*
 * Global headroom factor folded into every channel's gain, matching the
 * upstream SPESSASYNTH_GAIN_FACTOR. This attenuates the mix by ~4.4 dB to
 * leave room for the effect buses before the output clips.
 */
#define SS_GAIN_FACTOR 0.6f

/* From the channel files. */
void ss_channel_all_sound_off(SS_MIDIChannel *ch);

/* ── updateInternalParams ────────────────────────────────────────────────── */

void ss_channel_update_internal_params(SS_MIDIChannel *ch) {
	if(!ch) return;
	const SS_Processor *p = ch->synth;
	const bool drum = ch->drum_channel;

	const SS_ChannelSystemParameter *cs = &ch->system_params;
	const SS_ChannelMIDIParameter *cm = &ch->midi_params;

	/* Global layers fall back to neutral values when the channel is detached. */
	const float gs_gain = p ? p->system_params.gain : 1.0f;
	const float gm_gain = p ? p->midi_params.gain : 1.0f;
	const float gs_pan = p ? p->system_params.pan : 0.0f;
	const float gm_pan = p ? p->midi_params.pan : 0.0f;
	const float gs_key = p ? p->system_params.key_shift : 0.0f;
	const float gm_key = p ? p->midi_params.key_shift : 0.0f;
	const float gs_tune = p ? p->system_params.fine_tune : 0.0f;
	const float gm_tune = p ? p->midi_params.fine_tune : 0.0f;

	/* Tuning in cents — global layers are ignored for drum channels. */
	ch->current_tuning =
	(drum ? 0.0f : gs_tune) +
	(drum ? 0.0f : gm_tune) +
	cs->fine_tune +
	cm->fine_tune;

	/* Panning — normalized [-1;1] layers scaled into the -500..500 range. */
	ch->current_pan = (gs_pan + gm_pan + cs->pan) * 500.0f;
	/* Per-channel MIDI pan is the CC#10 controller, applied via modulators. */

	/* Key shift in semitones — global layers are ignored for drum channels. */
	ch->current_key_shift = (int)floorf(
	(drum ? 0.0f : gs_key) +
	(drum ? 0.0f : gm_key) +
	cs->key_shift +
	cm->key_shift);

	/* Output gain — multiplicative across every layer. */
	ch->current_gain = SS_GAIN_FACTOR * gs_gain * gm_gain * cs->gain;
	/* Per-channel MIDI gain is the volume/expression controllers. */
}

/* ── Channel parameter resets ────────────────────────────────────────────── */

/* Resets the per-channel system parameters to their defaults.
 * System parameters are API-only and therefore survive a MIDI reset; this
 * is only used when a channel is first created. */
void ss_channel_reset_system_parameters(SS_MIDIChannel *ch) {
	ch->system_params.preset_lock = false;
	ch->system_params.is_muted = false;
	ch->system_params.gain = 1.0f;
	ch->system_params.pan = 0.0f;
	ch->system_params.key_shift = 0.0f;
	ch->system_params.fine_tune = 0.0f;
	ch->system_params.interpolation_type = SS_PARAM_UNSET;
	ch->system_params.custom_vibrato_lock = SS_PARAM_UNSET;
	ch->system_params.nrpn_param_lock = SS_PARAM_UNSET;
	ch->system_params.monophonic_retrigger = SS_PARAM_UNSET;
}

/* Resets the per-channel MIDI parameters to their defaults. */
void ss_channel_reset_midi_parameters(SS_MIDIChannel *ch) {
	ch->midi_params.rx_channel = ch->channel_number;
	ch->midi_params.poly_mode = true;
	ch->midi_params.fine_tune = 0.0f;
	ch->midi_params.key_shift = 0.0f;
	ch->midi_params.random_pan = false;
	ch->midi_params.assign_mode = 2;
	ch->midi_params.efx_assign = false;
	ch->midi_params.cc1 = 0x10;
	ch->midi_params.cc2 = 0x11;
	ch->midi_params.drum_map = 0;
}

/* ── Channel parameter setters ───────────────────────────────────────────── */

void ss_channel_set_system_parameter(SS_MIDIChannel *ch,
                                     SS_ChannelSystemParameterType param,
                                     double value) {
	if(!ch) return;
	SS_ChannelSystemParameter *sp = &ch->system_params;
	switch(param) {
		case SS_CHANNEL_SYS_PRESET_LOCK:
			sp->preset_lock = value != 0.0;
			break;
		case SS_CHANNEL_SYS_IS_MUTED:
			sp->is_muted = value != 0.0;
			/* Silence the channel immediately when muting. */
			if(sp->is_muted) ss_channel_all_sound_off(ch);
			break;
		case SS_CHANNEL_SYS_GAIN:
			sp->gain = (float)value;
			break;
		case SS_CHANNEL_SYS_PAN:
			sp->pan = (float)value;
			break;
		case SS_CHANNEL_SYS_KEY_SHIFT: {
			const float prev = sp->key_shift;
			sp->key_shift = (float)value;
			/* A key shift change re-pitches sample selection. */
			if(!ch->drum_channel && prev != sp->key_shift)
				ss_channel_all_sound_off(ch);
			break;
		}
		case SS_CHANNEL_SYS_FINE_TUNE:
			sp->fine_tune = (float)value;
			break;
		case SS_CHANNEL_SYS_INTERPOLATION_TYPE:
			sp->interpolation_type = (int)value;
			break;
		case SS_CHANNEL_SYS_CUSTOM_VIBRATO_LOCK:
			sp->custom_vibrato_lock = (int8_t)value;
			break;
		case SS_CHANNEL_SYS_NRPN_PARAM_LOCK:
			sp->nrpn_param_lock = (int8_t)value;
			break;
		case SS_CHANNEL_SYS_MONOPHONIC_RETRIGGER:
			sp->monophonic_retrigger = (int8_t)value;
			break;
	}
	ss_channel_update_internal_params(ch);
}

void ss_channel_set_midi_parameter(SS_MIDIChannel *ch,
                                   SS_ChannelMIDIParameterType param,
                                   double value) {
	if(!ch) return;
	SS_ChannelMIDIParameter *mp = &ch->midi_params;
	switch(param) {
		case SS_CHANNEL_MIDI_RX_CHANNEL:
			mp->rx_channel = (int)value;
			break;
		case SS_CHANNEL_MIDI_POLY_MODE:
			mp->poly_mode = value != 0.0;
			break;
		case SS_CHANNEL_MIDI_FINE_TUNE:
			mp->fine_tune = (float)value;
			break;
		case SS_CHANNEL_MIDI_KEY_SHIFT:
			/* Drum channels ignore key shift. */
			mp->key_shift = ch->drum_channel ? 0.0f : (float)value;
			break;
		case SS_CHANNEL_MIDI_RANDOM_PAN:
			mp->random_pan = value != 0.0;
			break;
		case SS_CHANNEL_MIDI_ASSIGN_MODE:
			mp->assign_mode = (int)value;
			break;
		case SS_CHANNEL_MIDI_EFX_ASSIGN:
			mp->efx_assign = value != 0.0;
			break;
		case SS_CHANNEL_MIDI_CC1:
			mp->cc1 = (uint8_t)value;
			break;
		case SS_CHANNEL_MIDI_CC2:
			mp->cc2 = (uint8_t)value;
			break;
		case SS_CHANNEL_MIDI_DRUM_MAP:
			mp->drum_map = (uint8_t)value;
			break;
	}
	ss_channel_update_internal_params(ch);
}

/* ── Global parameter resets ─────────────────────────────────────────────── */

/* Initializes the global parameters when a processor is created. */
void ss_processor_init_parameters(SS_Processor *proc) {
	SS_GlobalSystemParameter *sp = &proc->system_params;
	sp->effects_enabled = proc->options.enable_effects;
	sp->events_enabled = true;
	sp->voice_cap = (int)proc->options.voice_cap;
	sp->auto_allocate_voices = false;
	sp->reverb_gain = 1.0f;
	sp->reverb_lock = false;
	sp->chorus_gain = 1.0f;
	sp->chorus_lock = false;
	sp->delay_gain = 1.0f;
	sp->delay_lock = false;
	sp->insertion_effect_lock = false;
	sp->drum_lock = false;
	sp->black_midi_mode = false;
	sp->device_id = -1;
	sp->gain = 1.0f;
	sp->pan = 0.0f;
	sp->key_shift = 0.0f;
	sp->fine_tune = 0.0f;
	sp->interpolation_type = proc->options.interpolation;
	sp->custom_vibrato_lock = false;
	sp->nrpn_param_lock = false;
	sp->monophonic_retrigger = false;

	proc->midi_params.system = SS_SYSTEM_GS;
	proc->midi_params.key_shift = 0.0f;
	proc->midi_params.fine_tune = 0.0f;
	proc->midi_params.gain = 1.0f;
	proc->midi_params.pan = 0.0f;
}

/* ── Global parameter setters ────────────────────────────────────────────── */

static void proc_update_all_channels(SS_Processor *proc) {
	for(int i = 0; i < proc->channel_count; i++) {
		if(proc->midi_channels[i])
			ss_channel_update_internal_params(proc->midi_channels[i]);
	}
}

void ss_processor_set_system_parameter(SS_Processor *proc,
                                       SS_GlobalSystemParameterType param,
                                       double value) {
	if(!proc) return;
	SS_GlobalSystemParameter *sp = &proc->system_params;
	switch(param) {
		case SS_GLOBAL_SYS_EFFECTS_ENABLED:
			sp->effects_enabled = value != 0.0;
			break;
		case SS_GLOBAL_SYS_EVENTS_ENABLED:
			sp->events_enabled = value != 0.0;
			break;
		case SS_GLOBAL_SYS_VOICE_CAP:
			sp->voice_cap = (int)value;
			break;
		case SS_GLOBAL_SYS_AUTO_ALLOCATE_VOICES:
			sp->auto_allocate_voices = value != 0.0;
			break;
		case SS_GLOBAL_SYS_REVERB_GAIN:
			sp->reverb_gain = (float)value;
			break;
		case SS_GLOBAL_SYS_REVERB_LOCK:
			sp->reverb_lock = value != 0.0;
			break;
		case SS_GLOBAL_SYS_CHORUS_GAIN:
			sp->chorus_gain = (float)value;
			break;
		case SS_GLOBAL_SYS_CHORUS_LOCK:
			sp->chorus_lock = value != 0.0;
			break;
		case SS_GLOBAL_SYS_DELAY_GAIN:
			sp->delay_gain = (float)value;
			break;
		case SS_GLOBAL_SYS_DELAY_LOCK:
			sp->delay_lock = value != 0.0;
			break;
		case SS_GLOBAL_SYS_INSERTION_EFFECT_LOCK:
			sp->insertion_effect_lock = value != 0.0;
			break;
		case SS_GLOBAL_SYS_DRUM_LOCK:
			sp->drum_lock = value != 0.0;
			break;
		case SS_GLOBAL_SYS_BLACK_MIDI_MODE:
			sp->black_midi_mode = value != 0.0;
			break;
		case SS_GLOBAL_SYS_DEVICE_ID:
			sp->device_id = (int)value;
			break;
		case SS_GLOBAL_SYS_GAIN:
			sp->gain = (float)value;
			break;
		case SS_GLOBAL_SYS_PAN:
			sp->pan = (float)value;
			break;
		case SS_GLOBAL_SYS_KEY_SHIFT: {
			const float prev = sp->key_shift;
			sp->key_shift = (float)value;
			if(prev != sp->key_shift) {
				for(int i = 0; i < proc->channel_count; i++)
					if(proc->midi_channels[i])
						ss_channel_all_sound_off(proc->midi_channels[i]);
			}
			break;
		}
		case SS_GLOBAL_SYS_FINE_TUNE:
			sp->fine_tune = (float)value;
			break;
		case SS_GLOBAL_SYS_INTERPOLATION_TYPE:
			sp->interpolation_type = (SS_InterpolationType)(int)value;
			break;
		case SS_GLOBAL_SYS_CUSTOM_VIBRATO_LOCK:
			sp->custom_vibrato_lock = value != 0.0;
			break;
		case SS_GLOBAL_SYS_NRPN_PARAM_LOCK:
			sp->nrpn_param_lock = value != 0.0;
			break;
		case SS_GLOBAL_SYS_MONOPHONIC_RETRIGGER:
			sp->monophonic_retrigger = value != 0.0;
			break;
	}
	proc_update_all_channels(proc);
}

void ss_processor_set_midi_parameter(SS_Processor *proc,
                                     SS_GlobalMIDIParameterType param,
                                     double value) {
	if(!proc) return;
	switch(param) {
		case SS_GLOBAL_MIDI_SYSTEM:
			proc->midi_params.system = (SS_MIDISystem)(int)value;
			break;
		case SS_GLOBAL_MIDI_KEY_SHIFT:
			proc->midi_params.key_shift = (float)value;
			break;
		case SS_GLOBAL_MIDI_FINE_TUNE:
			proc->midi_params.fine_tune = (float)value;
			break;
		case SS_GLOBAL_MIDI_GAIN:
			proc->midi_params.gain = (float)value;
			break;
		case SS_GLOBAL_MIDI_PAN:
			proc->midi_params.pan = (float)value;
			break;
	}
	proc_update_all_channels(proc);
}
