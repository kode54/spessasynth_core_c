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

void ss_lowpass_filter_init(SS_LowpassFilter *f, uint32_t sample_rate);
void ss_lowpass_filter_apply(SS_LowpassFilter *f,
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

void ss_dynamic_modulator_system_init(SS_DynamicModulatorSystem *dms);
void ss_dynamic_modulator_system_free(SS_DynamicModulatorSystem *dms);
void ss_dynamic_modulator_system_setup_receiver(SS_DynamicModulatorSystem *dms,
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
	int real_key;

	double start_time;
	double release_start_time; /* INFINITY = not yet released */

	int current_tuning_cents;
	double current_tuning_calculated;
	float current_pan;
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

SS_Voice *ss_voice_create(uint32_t sample_rate,
                          const SS_BasicPreset *preset,
                          const SS_AudioSample *audio_sample,
                          int midi_note, int velocity,
                          double current_time, int target_key, int real_key,
                          const int16_t *generators,
                          const SS_Modulator *modulators, size_t mod_count,
                          const SS_DynamicModulatorSystem *dms);
/*SS_Voice *ss_voice_copy(const SS_Voice *src, double current_time, int real_key);*/
void ss_voice_free(SS_Voice *v);

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

typedef enum {
	SS_DATAENTRY_IDLE = 0,
	SS_DATAENTRY_RP_COARSE = 1,
	SS_DATAENTRY_RP_FINE = 2,
	SS_DATAENTRY_NRP_COARSE = 3,
	SS_DATAENTRY_NRP_FINE = 4,
	SS_DATAENTRY_DATA_COARSE = 5,
	SS_DATAENTRY_DATA_FINE = 6
} SS_DataEntryState;

enum { GENERATOR_OVERRIDE_NO_CHANGE_VALUE = 32767 };

typedef struct {
	float delay;
	float depth;
	float rate;
} SS_ChannelVibrato;

struct SS_Processor; /* forward */

typedef struct SS_MIDIChannel {
	int16_t midi_controllers[SS_MIDI_CONTROLLER_COUNT];
	bool locked_controllers[SS_MIDI_CONTROLLER_COUNT];
	float custom_controllers[SS_CUSTOM_CTRL_COUNT];

	SS_DataEntryState data_entry_state;

	SS_DynamicModulatorSystem dms;

	int channel_transpose_key_shift;
	int8_t channel_octave_tuning[128];
	int channel_tuning_cents;
	int16_t generator_offsets[SS_GEN_COUNT];
	bool generator_offsets_enabled;
	int16_t generator_overrides[SS_GEN_COUNT];
	bool generator_overrides_enabled;

	bool drum_channel;
	bool random_pan;
	bool is_muted;
	bool poly_mode; /* true = polyphonic (default), false = monophonic */
	bool insertion_enabled; /* GS EFX assign: route voices to insertion processor */
	/**
	 * Assign mode for the channel.
	 * ASSIGN MODE is the parameter that determines how voice assignment will be handled when sounds overlap on identical note numbers in the same channel (i.e., repeatedly struck notes).
	 * This is initialized to a mode suitable for each Part, so for general purposes there is no need to change this.
	 *
	 * 0 - Single: If the same note is played multiple times in succession, the previously-sounding note will be completely silenced, and then the new note will be sounded.
	 * 1 - LimitedMulti: If the same note is played multiple times in succession, the previously-sounding note will be continued to a certain extent even after the new note is sounded. (Default setting)
	 * 2 - FullMulti: If the same note is played multiple times in succession, the previously-sounding note(s) will continue sounding for their natural length even after the new note is sounded.
	 * We treat LimitedMulti like FullMulti
	 */
	int assign_mode;

	uint8_t bank_msb;
	uint8_t bank_lsb;
	uint8_t program;
	bool is_gm_gs_drum;
	uint8_t drum_map; /* GS drum map value (0 = none) */
	/**
	 * CC1 for GS controller matrix.
	 * An arbitrary MIDI controller, which can be bound to any synthesis parameter.
	 * Default is 16
	 */
	uint8_t cc1;
	/**
	 * CC2 for GS controller matrix.
	 * An arbitrary MIDI controller, which can be bound to any synthesis parameter.
	 *  * Default is 17
	 */
	uint8_t cc2;
	int rx_channel; /* receive channel override (-1 = off), default = channel_number */

	SS_SoundBank *bank; /* non-owning */
	SS_BasicPreset *preset; /* non-owning */
	bool lock_preset;

	SS_ChannelVibrato channel_vibrato;

	SS_DrumParameters drum_params[128]; /* per-key drum parameters */

	bool per_note_pitch; /* true when MIDI 2.0 per-note pitch wheel is active */
	int16_t pitch_wheels[128]; /* per-note pitch wheel values (0..16383, 8192 = center) */

	SS_Voice **voices; /* owned */
	size_t voice_count;
	size_t voice_capacity;

	SS_Voice **sustained_voices; /* non-owning, points into voices */
	size_t sustained_count;
	size_t sustained_capacity;

	int channel_number;
	struct SS_Processor *synth; /* non-owning back-pointer */
} SS_MIDIChannel;

SS_MIDIChannel *ss_channel_new(int channel_number, struct SS_Processor *synth);
void ss_channel_free(SS_MIDIChannel *ch);
void ss_channel_note_on(SS_MIDIChannel *ch, int note, int vel, double time);
void ss_channel_note_off(SS_MIDIChannel *ch, int note, double time);
void ss_channel_all_notes_off(SS_MIDIChannel *ch, double time);
void ss_channel_all_sound_off(SS_MIDIChannel *ch);
void ss_channel_controller(SS_MIDIChannel *ch, int cc, int val, double time);
void ss_channel_program_change(SS_MIDIChannel *ch, int program);
void ss_channel_pitch_wheel(SS_MIDIChannel *ch, int value, int midi_note, double time);
void ss_channel_reset_controllers(SS_MIDIChannel *ch);

/**
 * Render all voices in this channel into output buffers.
 * out_left/out_right/reverb_left/reverb_right/chorus_left/chorus_right are float
 * buffers of length sample_count.  They are mixed into (not cleared).
 */
void ss_channel_render(SS_MIDIChannel *ch,
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

/* ── Master parameters ────────────────────────────────────────────────────── */

typedef enum {
	SS_SYSTEM_GM = 0,
	SS_SYSTEM_GS = 1,
	SS_SYSTEM_XG = 2
} SS_MIDISystem;

typedef struct {
	float master_volume; /* 0 to 1 */
	float master_pan; /* -1 to +1 */
	float master_pitch; /* semitones */
	float master_tuning; /* cents */
	float delay_gain;
	SS_InterpolationType interpolation_type;
	SS_MIDISystem midi_system;
	bool reverb_enabled;
	bool chorus_enabled;
	bool delay_enabled;
	SS_TuningEntry **tunings; /* [128][128] tuning grid, or NULL */
} SS_MasterParameters;

/* ── Processor (synthesis engine) ────────────────────────────────────────── */

#define SS_MAX_SOUND_CHUNK 128

/* Registered bank group: one string ID → one SS_FilteredBanks (which
 * may internally contain many filtered banks, as produced by an sflist). */
typedef struct SS_ProcessorBankGroup {
	char *id;                  /* OWNED */
	SS_FilteredBanks *banks;   /* OWNED when external_banks=false; non-owning view otherwise */
	bool external_banks;       /* true = caller retains ownership of the SS_SoundBank(s) */
} SS_ProcessorBankGroup;

typedef struct SS_Processor {
	uint32_t sample_rate;
	SS_MIDIChannel *midi_channels[SS_CHANNEL_COUNT * 4]; /* up to 4 ports */
	int channel_count;

	SS_ProcessorBankGroup *bank_groups; /* registered banks, in search order */
	size_t bank_group_count;
	size_t bank_group_allocated;

	int total_voices;
	double current_synth_time; /* seconds */

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

	SS_MasterParameters master_params;
	float midi_volume, pan_left, pan_right;
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
SS_Processor *ss_processor_create(uint32_t sample_rate,
                                  const SS_ProcessorOptions *opts);
void ss_processor_free(SS_Processor *proc);

SS_SoundBank *ss_processor_get_soundbank(SS_Processor *proc, const char *id);
bool ss_processor_load_soundbank(SS_Processor *proc,
                                 SS_SoundBank *bank, const char *id, int offset,
                                 bool insert);
/**
 * Register a pre-built SS_FilteredBanks (e.g. from sflist_load) under an ID.
 * Ownership of `banks` transfers to the processor on success.  On removal,
 * the underlying SS_SoundBanks are freed unless remove is called with
 * dontfree=true.
 */
bool ss_processor_load_filtered_banks(SS_Processor *proc,
                                      SS_FilteredBanks *banks, const char *id,
                                      bool insert);
bool ss_processor_remove_soundbank(SS_Processor *proc, const char *id, bool dontfree);

/**
 * Resolve a preset across all currently registered bank groups, honoring
 * each filtered bank's channel range.  target_channel may be -1 to
 * ignore channel filtering.
 */
SS_BasicPreset *ss_processor_resolve_preset(SS_Processor *proc,
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
void ss_processor_render(SS_Processor *proc,
                         float *out_left, float *out_right,
                         uint32_t sample_count);

void ss_processor_render_interleaved(SS_Processor *proc,
                                     float *out, uint32_t sample_count);
/**
 * These event functions all accept an absolute timestamp, `t`, which is in seconds
 * elapsed since the creation of the SS_Processor.
 */
void ss_processor_note_on(SS_Processor *proc, int ch, int note, int vel, double t);
void ss_processor_note_off(SS_Processor *proc, int ch, int note, double t);
void ss_processor_control_change(SS_Processor *proc, int ch, int cc, int val, double t);
void ss_processor_program_change(SS_Processor *proc, int ch, int program, double t);
void ss_processor_pitch_wheel(SS_Processor *proc, int ch, int value, int midi_note, double t);
void ss_processor_channel_pressure(SS_Processor *proc, int ch, int pressure, double t);
void ss_processor_poly_pressure(SS_Processor *proc, int ch, int note, int pressure, double t);

/*
 * This function takes a pointer to the inner message, not counting the leading 0xf0
 * or trailing 0xf7. It is up to the caller to validate that those are present in the
 * source, but exclude them when passing the buffer and size to this message.
 */
void ss_processor_sysex(SS_Processor *proc, const uint8_t *data, size_t len, double t);

/**
 * This reset takes place immediately, but does not reset the internal absolute
 * time position of the synthesizer, which counts up monotonically every time
 * samples are rendered.
 */
void ss_processor_system_reset(SS_Processor *proc);

/**
 * This optional callback will receive events every time either the above event
 * functions are used, or every time a SysEx or N/RPN message triggers one of
 * the above events.
 */
void ss_processor_set_event_callback(SS_Processor *proc,
                                     SS_EventCallback cb, void *userdata);

/**
 * One time initialization. It is best if this is called manually, especially
 * if multiple instances of the Processor are ever used in different threads.
 */
void ss_unit_converter_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SS_SYNTH_H */
