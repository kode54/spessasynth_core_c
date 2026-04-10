#ifndef SS_SOUNDBANK_ENUMS_H
#define SS_SOUNDBANK_ENUMS_H

#if __has_include(<spessasynth_core/generator_types.h>)
#include <spessasynth_core/generator_types.h>
#else
#include "spessasynth/soundbank/generator_types.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	SS_SAMPLE_TYPE_MONO = 1,
	SS_SAMPLE_TYPE_RIGHT = 2,
	SS_SAMPLE_TYPE_LEFT = 4,
	SS_SAMPLE_TYPE_LINKED = 8,
	SS_SAMPLE_TYPE_ROM_MONO = 0x8001,
	SS_SAMPLE_TYPE_ROM_RIGHT = 0x8002,
	SS_SAMPLE_TYPE_ROM_LEFT = 0x8004,
	SS_SAMPLE_TYPE_ROM_LINKED = 0x8008
} SS_SampleType;

/* SF3 compression flag (bit 4 in the sampleType field) */
#define SS_SF3_COMPRESSED_FLAG 0x10u

typedef enum {
	SS_MODSRC_NO_CONTROLLER = 0,
	SS_MODSRC_NOTE_ON_VELOCITY = 2,
	SS_MODSRC_NOTE_ON_KEYNUM = 3,
	SS_MODSRC_POLY_PRESSURE = 10,
	SS_MODSRC_CHANNEL_PRESSURE = 13,
	SS_MODSRC_PITCH_WHEEL = 14,
	SS_MODSRC_PITCH_WHEEL_RANGE = 16,
	SS_MODSRC_LINK = 127
} SS_ModulatorSource;

typedef enum {
	SS_MODCURVE_LINEAR = 0,
	SS_MODCURVE_CONCAVE = 1,
	SS_MODCURVE_CONVEX = 2,
	SS_MODCURVE_SWITCH = 3
} SS_ModulatorCurveType;

typedef enum {
	SS_MODTRANS_LINEAR = 0,
	SS_MODTRANS_ABSOLUTE = 2
} SS_ModulatorTransformType;

// Source curve type maps to a soundfont curve type in section 2.10, table 9
typedef SS_ModulatorCurveType SS_DLSTransform;

typedef enum {
	SS_DLSSRC_NONE = 0x0,
	SS_DLSSRC_MODLFO = 0x1,
	SS_DLSSRC_VELOCITOY = 0x2,
	SS_DLSSRC_KEYNUM = 0x3,
	SS_DLSSRC_VOLENV = 0x4,
	SS_DLSSRC_MODENV = 0x5,
	SS_DLSSRC_PITCH_WHEEL = 0x6,
	SS_DLSSRC_POLY_PRESSURE = 0x7,
	SS_DLSSRC_CHANNEL_PRESSURE = 0x8,
	SS_DLSSRC_VIBRATO_LFO = 0x9,

	SS_DLSSRC_MODULATION_WHEEL = 0x81,
	SS_DLSSRC_VOLUME = 0x87,
	SS_DLSSRC_PAN = 0x8a,
	SS_DLSSRC_EXPRESSION = 0x8b,
	// Note: these are flipped unintentionally in DLS2 table 9. Argh!
	SS_DLSSRC_CHORUS = 0xdd,
	SS_DLSSRC_REVERB = 0xdb,

	SS_DLSSRC_PITCH_WHEEL_RANGE = 0x100,
	SS_DLSSRC_FINETUNE = 0x101,
	SS_DLSSRC_COARSETUNE = 0x102
} SS_DLSSource;

typedef enum {
	SS_DLSDEST_NONE = 0x0, // No destination
	SS_DLSDEST_GAIN = 0x1, // Linear gain
	SS_DLSDEST_RESERVED = 0x2, // Reserved
	SS_DLSDEST_PITCH = 0x3, // Pitch in cents
	SS_DLSDEST_PAN = 0x4, // Pan 10ths of a percent
	SS_DLSDEST_KEYNUM = 0x5, // MIDI key number
	// Nuh uh, the channel controllers are not supported!
	SS_DLSDEST_CHORUS_SEND = 0x80, // Chorus send level 10ths of a percent
	SS_DLSDEST_REVERB_SEND = 0x81, // Reverb send level 10ths of a percent

	SS_DLSDEST_MOD_LFO_FREQ = 0x104, // Modulation LFO frequency
	SS_DLSDEST_MOD_LFO_DELAY = 0x105, // Modulation LFO delay

	SS_DLSDEST_VIB_LFO_FREQ = 0x114, // Vibrato LFO frequency
	SS_DLSDEST_VIB_LFO_DELAY = 0x115, // Vibrato LFO delay

	SS_DLSDEST_VOL_ENV_ATTACK = 0x206, // Volume envelope attack
	SS_DLSDEST_VOL_ENV_DECAY = 0x207, // Volume envelope decay
	SS_DLSDEST_RESERVED_EG1 = 0x208, // Reserved
	SS_DLSDEST_VOL_ENV_RELEASE = 0x209, // Volume envelope release
	SS_DLSDEST_VOL_ENV_SUSTAIN = 0x20a, // Volume envelope sustain
	SS_DLSDEST_VOL_ENV_DELAY = 0x20b, // Volume envelope delay
	SS_DLSDEST_VOL_ENV_HOLD = 0x20c, // Volume envelope hold

	SS_DLSDEST_MOD_ENV_ATTACK = 0x30a, // Modulation envelope attack
	SS_DLSDEST_MOD_ENV_DECAY = 0x30b, // Modulation envelope decay
	SS_DLSDEST_RESERVED_EG2 = 0x30c, // Reserved
	SS_DLSDEST_MOD_ENV_RELEASE = 0x30d, // Modulation envelope release
	SS_DLSDEST_MOD_ENV_SUSTAIN = 0x30e, // Modulation envelope sustain
	SS_DLSDEST_MOD_ENV_DELAY = 0x30f, // Modulation envelope delay
	SS_DLSDEST_MOD_ENV_HOLD = 0x310, // Modulation envelope hold

	SS_DLSDEST_FILTER_CUTOFF = 0x500, // Low pass filter cutoff frequency
	SS_DLSDEST_FILTER_Q = 0x501 // Low pass filter resonance
} SS_DLSDestination;

typedef enum {
	SS_DLSLOOP_FORWARD = 0x0000,
	SS_DLSLOOP_LOOP_AND_RELEASE = 0x0001
} SS_DLSLoopType;

#ifdef __cplusplus
}
#endif

#endif /* SS_SOUNDBANK_ENUMS_H */
