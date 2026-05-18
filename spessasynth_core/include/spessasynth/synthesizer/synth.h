#ifndef SS_SYNTH_H
#define SS_SYNTH_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#if __has_include(<spessasynth_core/soundbank.h>)
#include <spessasynth_core/chorus.h>
#include <spessasynth_core/delay.h>
#include <spessasynth_core/insertion.h>
#include <spessasynth_core/reverb.h>
#include <spessasynth_core/soundbank.h>
#else
#include "../soundbank/soundbank.h"
#include "dsp/chorus.h"
#include "dsp/delay.h"
#include "dsp/insertion.h"
#include "dsp/reverb.h"
#endif

#ifdef _MSC_VER
#include "spessasynth_exports.h"
#else
#define SPESSASYNTH_EXPORTS
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Interpolation type ──────────────────────────────────────────────────── */

typedef enum {
	SS_INTERP_LINEAR = 0,
	SS_INTERP_NEAREST = 1,
	SS_INTERP_HERMITE = 2,
	SS_INTERP_SINC = 3
} SS_InterpolationType;

/* ── Sample looping mode ─────────────────────────────────────────────────── */

typedef enum {
	SS_LOOP_NONE = 0,
	SS_LOOP_LOOP = 1,
	SS_LOOP_START_RELEASE = 2, /* unofficial polyphone 2.4 */
	SS_LOOP_LOOP_RELEASE = 3 /* loop, then play to end on release */
} SS_SampleLoopingMode;

/* ── AudioSample (wavetable oscillator state) ────────────────────────────── */

typedef struct {
	const float *sample_data; /* non-owning ptr into SS_BasicSample.audio_data */
	size_t sample_data_len;
	float playback_step; /* base playback rate */
	double cursor; /* current read position (fractional) */
	int root_key;
	size_t loop_start;
	size_t loop_end;
	size_t end; /* last valid sample index */
	SS_SampleLoopingMode looping_mode;
	bool is_looping;
} SS_AudioSample;

/* ── Lowpass filter ──────────────────────────────────────────────────────── */

typedef struct {
	int resonance_cb;
	float current_initial_fc; /* current smoothed cutoff, abs cents */
	float last_target_cutoff;
	double a0, a1, a2, a3, a4; /* biquad coefficients */
	double x1, x2; /* input history */
	double y1, y2; /* output history */
	bool initialized;
	float max_cutoff; /* Hz: sample_rate * 0.45 */
	uint32_t sample_rate;
} SS_LowpassFilter;

void SPESSASYNTH_EXPORTS ss_lowpass_filter_init(SS_LowpassFilter *f, uint32_t sample_rate);
void SPESSASYNTH_EXPORTS ss_lowpass_filter_apply(SS_LowpassFilter *f,
                                                 const int16_t *modulated_generators,
                                                 float *buffer, int count,
                                                 float fc_excursion, float smoothing,
                                                 float gain, float gain_inc);

/* ── Volume envelope ─────────────────────────────────────────────────────── */

typedef enum {
	SS_VOLENV_DELAY = 0,
	SS_VOLENV_ATTACK = 1,
	SS_VOLENV_HOLD = 2,
	SS_VOLENV_DECAY = 3,
	SS_VOLENV_SUSTAIN = 4
} SS_VolumeEnvelopeState;

typedef struct {
	uint32_t sample_rate;
	uint32_t update_interval;
	float output_gain;
	double attenuation_cb;
	SS_VolumeEnvelopeState state;
	uint64_t sample_time;
	double release_start_cb;
	uint64_t release_start_time_samples;
	uint64_t attack_duration;
	uint64_t decay_duration;
	uint64_t release_duration;
	double sustain_cb;
	uint64_t delay_end;
	uint64_t attack_end;
	uint64_t hold_end;
	uint64_t decay_end;
	bool entered_release;
	bool can_end_on_silent_sustain;
} SS_VolumeEnvelope;

/* ── Modulation envelope ─────────────────────────────────────────────────── */

typedef struct {
	double attack_duration;
	double decay_duration;
	double hold_duration;
	double release_duration;
	float sustain_level;
	double delay_end;
	double attack_end;
	double hold_end;
	double decay_end;
	double release_start_time;
	float release_start_level;
	float current_value;
	bool entered_release;
} SS_ModulationEnvelope;

/* ── Per-key drum parameters (XG / GS) ───────────────────────────────────── */

typedef struct {
	float pitch; /* pitch offset in cents (default 0) */
	float gain; /* gain multiplier (default 1) */
	uint8_t exclusive_class; /* exclusive class override (default 0 = off) */
	uint8_t pan; /* pan 1-127 (64=center, 0=random), default 64 */
	uint8_t filter_cutoff; /* filter cutoff frequency, default 64 */
	uint8_t filter_resonance; /* filter resonance, default 0 */
	float reverb_gain; /* reverb send multiplier (default 0) */
	float chorus_gain; /* chorus send multiplier (default 1) */
	float delay_gain; /* delay send multiplier (default 1) */
	bool rx_note_on; /* receive note on (default true) */
	bool rx_note_off; /* receive note off, fast-kill on off (default false) */
} SS_DrumParameters;

/* ── Dynamic Modulator System ────────────────────────────────────────────── */

typedef struct {
	SS_Modulator modulator;
	uint16_t source;
	uint16_t destination;
	bool is_bipolar;
	bool is_negative;
} SS_DynamicModulatorSystem_Modulator;

typedef struct {
	SS_DynamicModulatorSystem_Modulator *modulators;
	size_t modulator_count;
	size_t modulators_allocated;
	bool is_active;
} SS_DynamicModulatorSystem;

void SPESSASYNTH_EXPORTS ss_dynamic_modulator_system_init(SS_DynamicModulatorSystem *dms);
void SPESSASYNTH_EXPORTS ss_dynamic_modulator_system_free(SS_DynamicModulatorSystem *dms);
void SPESSASYNTH_EXPORTS ss_dynamic_modulator_system_setup_receiver(SS_DynamicModulatorSystem *dms,
                                                                    uint8_t addr3, uint8_t data,
                                                                    uint16_t source, bool is_bipolar);

/* ── Voice ───────────────────────────────────────────────────────────────── */

typedef struct SS_Voice {
	SS_AudioSample sample;
	SS_LowpassFilter filter;
	SS_VolumeEnvelope volume_env;
	SS_ModulationEnvelope modulation_env;

	const SS_BasicPreset *preset; /* non-owning */

	float gain;
	int16_t generators[SS_GEN_COUNT];
	int16_t modulated_generators[SS_GEN_COUNT];

	SS_Modulator *modulators; /* owned copy */
	size_t modulator_count;

	float resonance_offset;
	bool is_active;
	bool is_in_release;
	bool has_rendered; /* set to true after the first render call */

	int velocity;
	int midi_note;
	int pressure;
	int target_key;
	int sound_bank_key;

	double start_time;
	double release_start_time; /* INFINITY = not yet released */

	int current_tuning_cents;
	double current_tuning_calculated;
	float current_pan;
	bool override_pan_active;
	float override_pan;

	int portamento_from_key; /* -1 = off */
	double portamento_duration;

	/* LFO triangle-wave phase accumulators [0,1), initialized to 0.25 */
	double vib_lfo_start_time;
	double mod_lfo_start_time;
	float vib_lfo_phase;
	float mod_lfo_phase;

	int exclusive_class;

	float pitch_offset; /* per-voice pitch offset in cents (drum params) */
	float reverb_send; /* per-voice reverb send multiplier */
	float chorus_send; /* per-voice chorus send multiplier */
	float delay_send; /* per-voice delay send multiplier */

	/**
	 * In timecents, where zero means disabled (use the modulatedGenerators table).
	 * Used for exclusive notes and killing notes.
	 */
	int override_release_vol_env;
} SS_Voice;

SS_Voice SPESSASYNTH_EXPORTS *ss_voice_create(uint32_t sample_rate,
                                              const SS_BasicPreset *preset,
                                              const SS_AudioSample *audio_sample,
                                              int midi_note, int velocity,
                                              double current_time, int target_key, int sound_bank_key,
                                              const int16_t *generators,
                                              const SS_Modulator *modulators, size_t mod_count,
                                              const SS_DynamicModulatorSystem *dms);
/*SS_Voice SPESSASYNTH_EXPORTS *ss_voice_copy(const SS_Voice *src, double current_time, int sound_bank_key);*/
void SPESSASYNTH_EXPORTS ss_voice_free(SS_Voice *v);

/* ── Custom controller indices ───────────────────────────────────────────── */

typedef enum {
	SS_CUSTOM_CTRL_TUNING = 0,
	SS_CUSTOM_CTRL_TRANSPOSE_FINE = 1,
	SS_CUSTOM_CTRL_MODULATION_MULTIPLIER = 2,
	SS_CUSTOM_CTRL_MASTER_TUNING = 3,
	SS_CUSTOM_CTRL_TUNING_SEMITONES = 4,
	SS_CUSTOM_CTRL_KEY_SHIFT = 5,
	SS_CUSTOM_CTRL_SF2_NRPN_GENERATOR_LSB = 6,
	SS_CUSTOM_CTRL_COUNT = 7
} SS_CustomController;

/* ── MIDI channel ────────────────────────────────────────────────────────── */

#define SS_MIDI_CONTROLLER_COUNT 147
#define SS_CHANNEL_COUNT 16

#define NON_CC_INDEX_OFFSET 128

enum { GENERATOR_OVERRIDE_NO_CHANGE_VALUE = 32767 };

typedef struct {
	float delay;
	float depth;
	float rate;
} SS_ChannelVibrato;

struct SS_Processor; /* forward */

/* ── System and MIDI parameters ───────────────────────────────────────────── */
/*
 * Parameters are organized into a 2x2 matrix, mirroring the upstream
 * TypeScript implementation:
 *
 *               | system (API only)        | MIDI (set by MIDI messages) |
 *   ------------+--------------------------+-----------------------------+
 *   global      | SS_GlobalSystemParameter | SS_GlobalMIDIParameter      |
 *   per-channel | SS_ChannelSystemParameter| SS_ChannelMIDIParameter     |
 *
 * "system" parameters can only be changed through the API.
 * "MIDI" parameters are set by MIDI messages (System Exclusive, RPN, ...).
 *
 * The shared numeric parameters (gain, pan, key_shift, fine_tune) of all
 * four structures are summed together by ss_channel_update_internal_params()
 * into the channel's current_* aggregate fields used during rendering.
 */

/* Tri-state value for nullable per-channel system overrides:
 * SS_PARAM_UNSET means "fall back to the global parameter". */
#define SS_PARAM_UNSET (-1)

/**
 * Global system parameters. These can only be changed through the API.
 */
typedef struct {
	bool effects_enabled; /* whether reverb/chorus/delay/insertion run */
	bool events_enabled; /* whether the event callback fires */
	int voice_cap; /* maximum number of simultaneous voices */
	bool auto_allocate_voices; /* allocate instead of stealing at the cap */

	float reverb_gain; /* 0..n, 1 == 100% reverb */
	bool reverb_lock; /* prevent MIDI edits of the reverb */
	float chorus_gain; /* 0..n, 1 == 100% chorus */
	bool chorus_lock; /* prevent MIDI edits of the chorus */
	float delay_gain; /* 0..n, 1 == 100% delay */
	bool delay_lock; /* prevent MIDI edits of the delay */
	bool insertion_effect_lock; /* prevent MIDI edits of the insertion EFX */
	bool drum_lock; /* prevent MIDI edits of the drum parameters */

	bool black_midi_mode; /* force note killing instead of releasing */
	int device_id; /* SysEx device ID, -1 accepts all */

	/* Shared with channel: summed into the channel current_* aggregates. */
	float gain; /* master gain, 1 == 100% */
	float pan; /* master pan, -1 left .. 1 right */
	float key_shift; /* global key shift in semitones (drums ignore) */
	float fine_tune; /* global tuning in cents (drums ignore) */

	SS_InterpolationType interpolation_type;
	bool custom_vibrato_lock; /* prevent applying the custom vibrato */
	bool nrpn_param_lock; /* prevent changing parameters via NRPN */
	bool monophonic_retrigger; /* MS GS Wavetable-style note retrigger */
} SS_GlobalSystemParameter;

/**
 * Global MIDI parameters. These are set by MIDI System Exclusive messages.
 */
typedef struct {
	SS_MIDISystem system; /* GM, GM2, GS, XG */
	float key_shift; /* global key shift in semitones (drums ignore) */
	float fine_tune; /* global tuning in cents (drums ignore) */
	float gain; /* global volume gain */
	float pan; /* global panning, -1 left .. 1 right */
} SS_GlobalMIDIParameter;

/**
 * Per-channel system parameters. These can only be changed through the API.
 * The nullable members use SS_PARAM_UNSET to fall back to the global value.
 */
typedef struct {
	bool preset_lock; /* prevent program changes on this channel */
	bool is_muted; /* whether the channel is muted */

	/* Shared with the synth: summed into the channel current_* aggregates. */
	float gain; /* channel gain, 1 == 100% */
	float pan; /* channel pan, -1 left .. 1 right */
	float key_shift; /* channel key shift in semitones */
	float fine_tune; /* channel tuning in cents */

	int interpolation_type; /* SS_InterpolationType, or SS_PARAM_UNSET */
	int8_t custom_vibrato_lock; /* tri-state, SS_PARAM_UNSET = inherit */
	int8_t nrpn_param_lock; /* tri-state, SS_PARAM_UNSET = inherit */
	int8_t monophonic_retrigger; /* tri-state, SS_PARAM_UNSET = inherit */
} SS_ChannelSystemParameter;

/**
 * Per-channel MIDI parameters. These are set by MIDI messages.
 *
 * Note: unlike the upstream TypeScript, pressure, pitch_wheel and
 * pitch_wheel_range remain in the channel's midi_controllers table, and
 * the modulation depth remains a custom controller, so they are not
 * duplicated here.
 */
typedef struct {
	int rx_channel; /* receive channel override (-1 == off) */
	bool poly_mode; /* true = polyphonic, false = monophonic */
	float fine_tune; /* RPN/SysEx fine tuning in cents */
	float key_shift; /* RPN#2/SysEx key shift in semitones */
	bool random_pan; /* random panning for every note */
	int assign_mode; /* voice assignment mode (0=Single, 2=FullMulti) */
	bool efx_assign; /* route voices through the insertion EFX */
	uint8_t cc1; /* CC1 for the GS controller matrix (default 0x10) */
	uint8_t cc2; /* CC2 for the GS controller matrix (default 0x11) */
	uint8_t drum_map; /* GS drum map for SysEx tracking */
} SS_ChannelMIDIParameter;

/* Selector enums for the parameter setter functions. */

typedef enum {
	SS_GLOBAL_SYS_EFFECTS_ENABLED = 0,
	SS_GLOBAL_SYS_EVENTS_ENABLED,
	SS_GLOBAL_SYS_VOICE_CAP,
	SS_GLOBAL_SYS_AUTO_ALLOCATE_VOICES,
	SS_GLOBAL_SYS_REVERB_GAIN,
	SS_GLOBAL_SYS_REVERB_LOCK,
	SS_GLOBAL_SYS_CHORUS_GAIN,
	SS_GLOBAL_SYS_CHORUS_LOCK,
	SS_GLOBAL_SYS_DELAY_GAIN,
	SS_GLOBAL_SYS_DELAY_LOCK,
	SS_GLOBAL_SYS_INSERTION_EFFECT_LOCK,
	SS_GLOBAL_SYS_DRUM_LOCK,
	SS_GLOBAL_SYS_BLACK_MIDI_MODE,
	SS_GLOBAL_SYS_DEVICE_ID,
	SS_GLOBAL_SYS_GAIN,
	SS_GLOBAL_SYS_PAN,
	SS_GLOBAL_SYS_KEY_SHIFT,
	SS_GLOBAL_SYS_FINE_TUNE,
	SS_GLOBAL_SYS_INTERPOLATION_TYPE,
	SS_GLOBAL_SYS_CUSTOM_VIBRATO_LOCK,
	SS_GLOBAL_SYS_NRPN_PARAM_LOCK,
	SS_GLOBAL_SYS_MONOPHONIC_RETRIGGER
} SS_GlobalSystemParameterType;

typedef enum {
	SS_GLOBAL_MIDI_SYSTEM = 0,
	SS_GLOBAL_MIDI_KEY_SHIFT,
	SS_GLOBAL_MIDI_FINE_TUNE,
	SS_GLOBAL_MIDI_GAIN,
	SS_GLOBAL_MIDI_PAN
} SS_GlobalMIDIParameterType;

typedef enum {
	SS_CHANNEL_SYS_PRESET_LOCK = 0,
	SS_CHANNEL_SYS_IS_MUTED,
	SS_CHANNEL_SYS_GAIN,
	SS_CHANNEL_SYS_PAN,
	SS_CHANNEL_SYS_KEY_SHIFT,
	SS_CHANNEL_SYS_FINE_TUNE,
	SS_CHANNEL_SYS_INTERPOLATION_TYPE,
	SS_CHANNEL_SYS_CUSTOM_VIBRATO_LOCK,
	SS_CHANNEL_SYS_NRPN_PARAM_LOCK,
	SS_CHANNEL_SYS_MONOPHONIC_RETRIGGER
} SS_ChannelSystemParameterType;

typedef enum {
	SS_CHANNEL_MIDI_RX_CHANNEL = 0,
	SS_CHANNEL_MIDI_POLY_MODE,
	SS_CHANNEL_MIDI_FINE_TUNE,
	SS_CHANNEL_MIDI_KEY_SHIFT,
	SS_CHANNEL_MIDI_RANDOM_PAN,
	SS_CHANNEL_MIDI_ASSIGN_MODE,
	SS_CHANNEL_MIDI_EFX_ASSIGN,
	SS_CHANNEL_MIDI_CC1,
	SS_CHANNEL_MIDI_CC2,
	SS_CHANNEL_MIDI_DRUM_MAP
} SS_ChannelMIDIParameterType;

typedef struct SS_MIDIChannel {
	int16_t midi_controllers[SS_MIDI_CONTROLLER_COUNT];
	bool locked_controllers[SS_MIDI_CONTROLLER_COUNT];
	float custom_controllers[SS_CUSTOM_CTRL_COUNT];

	bool last_parameter_is_registered;

	SS_DynamicModulatorSystem dms;

	int8_t channel_octave_tuning[128];
	int channel_tuning_cents;
	int16_t generator_offsets[SS_GEN_COUNT];
	bool generator_offsets_enabled;
	int16_t generator_overrides[SS_GEN_COUNT];
	bool generator_overrides_enabled;

	bool drum_channel;

	/**
	 * Per-channel system parameters (API only) and MIDI parameters
	 * (set by MIDI messages). See the parameter struct definitions above.
	 */
	SS_ChannelSystemParameter system_params;
	SS_ChannelMIDIParameter midi_params;

	/* Aggregates recomputed by ss_channel_update_internal_params() from the
	 * four parameter structs; consumed on the hot rendering path. */
	float current_gain; /* output gain multiplier */
	float current_pan; /* added pan offset in the -500..500 range */
	float current_tuning; /* added tuning in cents */
	int current_key_shift; /* added key shift in semitones */

	uint8_t bank_msb;
	uint8_t bank_lsb;
	uint8_t program;

	SS_SoundBank *bank; /* non-owning */
	SS_BasicPreset *preset; /* non-owning */

	SS_ChannelVibrato channel_vibrato;

	SS_DrumParameters drum_params[128]; /* per-key drum parameters */

	bool per_note_pitch; /* true when MIDI 2.0 per-note pitch wheel is active */
	int16_t pitch_wheels[128]; /* per-note pitch wheel values (0..16383, 8192 = center) */

	/* The last pressed note on this channel.
	 * -1 means none.
	 * This is not a standard paramter and is strictly internal,
	 * mostly because we don't want to send events for every note on message.
	 * It can be set with Portamento Control CC anyway.
	 */
	int8_t last_note;
	/* If the portamento should be executed once regardless of Portamento on/off.
	 * Adhering to the MIDI spec, CC#84 ignores on/off.
	 * This is also not a standard parameter for the same reason as `last_note`
	 */
	bool portamento_force;

	SS_Voice **voices; /* owned */
	size_t voice_count;
	size_t voice_capacity;

	SS_Voice **sustained_voices; /* non-owning, points into voices */
	size_t sustained_count;
	size_t sustained_capacity;

	int channel_number;
	struct SS_Processor *synth; /* non-owning back-pointer */
} SS_MIDIChannel;

SS_MIDIChannel SPESSASYNTH_EXPORTS *ss_channel_new(int channel_number, struct SS_Processor *synth);
void SPESSASYNTH_EXPORTS ss_channel_free(SS_MIDIChannel *ch);
void SPESSASYNTH_EXPORTS ss_channel_note_on(SS_MIDIChannel *ch, int note, int vel, double time);
void SPESSASYNTH_EXPORTS ss_channel_note_off(SS_MIDIChannel *ch, int note, double time);
void SPESSASYNTH_EXPORTS ss_channel_all_notes_off(SS_MIDIChannel *ch, double time);
void SPESSASYNTH_EXPORTS ss_channel_all_sound_off(SS_MIDIChannel *ch);
void SPESSASYNTH_EXPORTS ss_channel_controller(SS_MIDIChannel *ch, int cc, int val, double time);
void SPESSASYNTH_EXPORTS ss_channel_program_change(SS_MIDIChannel *ch, int program);
void SPESSASYNTH_EXPORTS ss_channel_pitch_wheel(SS_MIDIChannel *ch, int value, int midi_note, double time);
void SPESSASYNTH_EXPORTS ss_channel_reset(SS_MIDIChannel *ch);

/* ── Channel parameters ──────────────────────────────────────────────────── */

/**
 * Sets a per-channel system parameter (API only).
 * Boolean parameters treat any non-zero value as true; the nullable
 * tri-state parameters accept SS_PARAM_UNSET to inherit the global value.
 */
void SPESSASYNTH_EXPORTS ss_channel_set_system_parameter(SS_MIDIChannel *ch,
                                                         SS_ChannelSystemParameterType param,
                                                         double value);

/**
 * Sets a per-channel MIDI parameter. Normally driven by MIDI messages,
 * but also usable through the API.
 */
void SPESSASYNTH_EXPORTS ss_channel_set_midi_parameter(SS_MIDIChannel *ch,
                                                       SS_ChannelMIDIParameterType param,
                                                       double value);

/**
 * Recomputes the channel's current_gain/current_pan/current_tuning/
 * current_key_shift aggregates from the four parameter structs.
 * Called automatically by the parameter setters.
 */
void SPESSASYNTH_EXPORTS ss_channel_update_internal_params(SS_MIDIChannel *ch);

/**
 * Render all voices in this channel into output buffers.
 * out_left/out_right/reverb_left/reverb_right/chorus_left/chorus_right are float
 * buffers of length sample_count.  They are mixed into (not cleared).
 */
void SPESSASYNTH_EXPORTS ss_channel_render(SS_MIDIChannel *ch,
                                           double time_now,
                                           float *out_left, float *out_right,
                                           float *reverb,
                                           float *chorus,
                                           float *delay,
                                           uint32_t sample_count);

/* ── Synth processor options ─────────────────────────────────────────────── */

typedef struct {
	bool enable_effects;
	uint32_t voice_cap;
	SS_InterpolationType interpolation;
	/* When true, ss_processor_load_soundbank() and
	 * ss_processor_load_filtered_banks() decode every sample reachable
	 * through the bank's instrument zones before returning.  Trades a
	 * one-time load cost for a real-time-safe synthesis path (no
	 * lazy Vorbis/FLAC decode on the audio thread). */
	bool preload_all_samples;
	/* When true, preload a whole instrument on program change. Less
	 * aggressive an option than the above, and matches what upstream
	 * does by default. */
	bool preload_instruments;
} SS_ProcessorOptions;

/* ── Event callback ──────────────────────────────────────────────────────── */

typedef enum {
	SS_EVENT_NOTE_ON = 0,
	SS_EVENT_NOTE_OFF = 1,
	SS_EVENT_PITCH_WHEEL = 2,
	SS_EVENT_CONTROLLER_CHANGE = 3,
	SS_EVENT_PROGRAM_CHANGE = 4,
	SS_EVENT_DRUM_CHANGE = 5,
	SS_EVENT_PRESET_LIST_CHANGE = 6,
	SS_EVENT_STOP_ALL = 7,
	SS_EVENT_SYSEX = 8
} SS_SynthEventType;

typedef struct {
	SS_SynthEventType type;
	int channel;
	int value1;
	int value2;
} SS_SynthEvent;

typedef void (*SS_EventCallback)(const SS_SynthEvent *event, void *userdata);

/* ── Tuning entry (MIDI Tuning Standard) ─────────────────────────────────── */

typedef struct {
	int midi_note;
	float cent_tuning;
} SS_TuningEntry;

/* ── Processor (synthesis engine) ────────────────────────────────────────── */

#define SS_MAX_SOUND_CHUNK 128

/* Registered bank group: one string ID → one SS_FilteredBanks (which
 * may internally contain many filtered banks, as produced by an sflist). */
typedef struct SS_ProcessorBankGroup {
	char *id; /* OWNED */
	SS_FilteredBanks *banks; /* OWNED when external_banks=false; non-owning view otherwise */
	bool external_banks; /* true = caller retains ownership of the SS_SoundBank(s) */
} SS_ProcessorBankGroup;

typedef struct SS_Processor {
	uint32_t sample_rate;
	SS_MIDIChannel *midi_channels[SS_CHANNEL_COUNT * 4]; /* up to 4 ports */
	int channel_count;

	SS_ProcessorBankGroup *bank_groups; /* registered banks, in search order */
	size_t bank_group_count;
	size_t bank_group_allocated;

	int voice_count;
	double current_time; /* seconds */

	SS_Reverb *reverb;
	SS_Chorus *chorus;
	SS_Delay *delay;
	SS_InsertionProcessor *insertion; /* active insertion effect processor (owned) */

	float reverb_buffer[SS_MAX_SOUND_CHUNK];
	float chorus_buffer[SS_MAX_SOUND_CHUNK];
	float delay_buffer[SS_MAX_SOUND_CHUNK];
	float insertion_left[SS_MAX_SOUND_CHUNK]; /* per-block insertion input accumulation buffer */
	float insertion_right[SS_MAX_SOUND_CHUNK];

	float mix_buffer[SS_MAX_SOUND_CHUNK];

	float interleave_left[SS_MAX_SOUND_CHUNK];
	float interleave_right[SS_MAX_SOUND_CHUNK];

	/**
	 * Global system parameters (API only) and MIDI parameters
	 * (set by MIDI System Exclusive). See the parameter struct definitions.
	 */
	SS_GlobalSystemParameter system_params;
	SS_GlobalMIDIParameter midi_params;
	SS_TuningEntry **tunings; /* [128][128] MTS tuning grid, or NULL */
	float pan_left, pan_right;
	float volume_envelope_smoothing_factor;
	float filter_smoothing_factor;
	float pan_smoothing_factor;

	bool delay_active; /* whether the delay effect has been activated via sysex */
	bool custom_channel_numbers; /* whether any channel uses a non-default rx_channel */
	bool insertion_active; /* true once any channel has insertion_enabled */

	SS_EventCallback event_callback;
	void *event_userdata;

	SS_ProcessorOptions options;

	int port_select_channel_offset;
} SS_Processor;

/**
 * The processor is not really thread safe. Either the event functions, or the render
 * functions may be used from exactly one thread at a time, but not both simultaneously.
 * It is suggested to use the provided mutex object to guard calls to the event posting
 * functions to separate them from calls to the render functions. Also, rendering may
 * not be called multiple times from different threads, either.
 *
 * Actually, there is no guarantee what will happen if you do either of the above things
 * in practice, calling functions simultaneously. Undefined behavior! It's not designed
 * to expect the functions to overlap with each other. It may just explode spectacularly!
 *
 * The insert variable declares that the bank should be inserted at the top of the list.
 */
SS_Processor SPESSASYNTH_EXPORTS *ss_processor_create(uint32_t sample_rate,
                                                      const SS_ProcessorOptions *opts);
void SPESSASYNTH_EXPORTS ss_processor_free(SS_Processor *proc);

SS_SoundBank SPESSASYNTH_EXPORTS *ss_processor_get_soundbank(SS_Processor *proc, const char *id);
bool SPESSASYNTH_EXPORTS ss_processor_load_soundbank(SS_Processor *proc,
                                                     SS_SoundBank *bank, const char *id, int offset,
                                                     bool insert);
/**
 * Register a pre-built SS_FilteredBanks (e.g. from sflist_load) under an ID.
 * Ownership of `banks` transfers to the processor on success.  On removal,
 * the underlying SS_SoundBanks are freed unless remove is called with
 * dontfree=true.
 */
bool SPESSASYNTH_EXPORTS ss_processor_load_filtered_banks(SS_Processor *proc,
                                                          SS_FilteredBanks *banks, const char *id,
                                                          bool insert);
bool SPESSASYNTH_EXPORTS ss_processor_remove_soundbank(SS_Processor *proc, const char *id, bool dontfree);

/**
 * Resolve a preset across all currently registered bank groups, honoring
 * each filtered bank's channel range.  target_channel may be -1 to
 * ignore channel filtering.
 */
SS_BasicPreset SPESSASYNTH_EXPORTS *ss_processor_resolve_preset(SS_Processor *proc,
                                                                int target_channel,
                                                                uint8_t program,
                                                                uint16_t bank_msb,
                                                                uint16_t bank_lsb,
                                                                bool is_drum);

/**
 * Main render call. Mixes into the provided float buffers.
 *
 * Due to how the timing of LFOs, Vibrato/Tremolo, etc. work, it is best to always
 * render in increments of the SS_MAX_SOUND_CHUNK unit declared elsewhere in this
 * file. It will always mix in increments of that, but will mix less if requested.
 *
 * Instead, render in increments of that sample count, and use finer offsets to the
 * timestamp parameter to the event functions.
 *
 * These functions clear the buffer automatically first.
 */
void SPESSASYNTH_EXPORTS ss_processor_render(SS_Processor *proc,
                                             float *out_left, float *out_right,
                                             uint32_t sample_count);

void SPESSASYNTH_EXPORTS ss_processor_render_interleaved(SS_Processor *proc,
                                                         float *out, uint32_t sample_count);
/**
 * These event functions all accept an absolute timestamp, `t`, which is in seconds
 * elapsed since the creation of the SS_Processor.
 */
void SPESSASYNTH_EXPORTS ss_processor_note_on(SS_Processor *proc, int ch, int note, int vel, double t);
void SPESSASYNTH_EXPORTS ss_processor_note_off(SS_Processor *proc, int ch, int note, double t);
void SPESSASYNTH_EXPORTS ss_processor_control_change(SS_Processor *proc, int ch, int cc, int val, double t);
void SPESSASYNTH_EXPORTS ss_processor_program_change(SS_Processor *proc, int ch, int program, double t);
void SPESSASYNTH_EXPORTS ss_processor_pitch_wheel(SS_Processor *proc, int ch, int value, int midi_note, double t);
void SPESSASYNTH_EXPORTS ss_processor_channel_pressure(SS_Processor *proc, int ch, int pressure, double t);
void SPESSASYNTH_EXPORTS ss_processor_poly_pressure(SS_Processor *proc, int ch, int note, int pressure, double t);

/*
 * This function takes a pointer to the inner message, not counting the leading 0xf0
 * or trailing 0xf7. It is up to the caller to validate that those are present in the
 * source, but exclude them when passing the buffer and size to this message.
 */
void SPESSASYNTH_EXPORTS ss_processor_sysex(SS_Processor *proc, const uint8_t *data, size_t len, double t);

/**
 * This reset takes place immediately, but does not reset the internal absolute
 * time position of the synthesizer, which counts up monotonically every time
 * samples are rendered.
 */
void SPESSASYNTH_EXPORTS ss_processor_system_reset(SS_Processor *proc);

/* ── Global parameters ───────────────────────────────────────────────────── */

/**
 * Sets a global system parameter (API only).
 * Boolean parameters treat any non-zero value as true.
 */
void SPESSASYNTH_EXPORTS ss_processor_set_system_parameter(SS_Processor *proc,
                                                           SS_GlobalSystemParameterType param,
                                                           double value);

/**
 * Sets a global MIDI parameter. Normally driven by MIDI System Exclusive
 * messages, but also usable through the API.
 */
void SPESSASYNTH_EXPORTS ss_processor_set_midi_parameter(SS_Processor *proc,
                                                         SS_GlobalMIDIParameterType param,
                                                         double value);

/**
 * This optional callback will receive events every time either the above event
 * functions are used, or every time a SysEx or N/RPN message triggers one of
 * the above events.
 */
void SPESSASYNTH_EXPORTS ss_processor_set_event_callback(SS_Processor *proc,
                                                         SS_EventCallback cb, void *userdata);

/**
 * One time initialization. It is best if this is called manually, especially
 * if multiple instances of the Processor are ever used in different threads.
 */
void SPESSASYNTH_EXPORTS ss_unit_converter_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SS_SYNTH_H */
