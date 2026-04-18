/*
 *  dynamic_modulators.c
 *  spessasynth_core
 *
 *  Created by Christopher Snowhill on 4/17/26.
 */

#include <stdint.h>

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/midi_enums.h>
#include <spessasynth_core/soundbank_enums.h>
#include <spessasynth_core/synth.h>
#else
#include "spessasynth/midi/midi_enums.h"
#include "spessasynth/soundbank/soundbank_enums.h"
#include "spessasynth/synthesizer/synth.h"
#endif

const SS_Modulator INITIAL_MODULATORS[] = {
	/* Vibrato rate to that one GS rate (in bare Hz) map for special cases such as J-Cycle.mid */
	MODULATOR(
	MODSRC(
	SS_MODCURVE_LINEAR,
	true,
	false,
	true,
	SS_MIDCON_VIBRATO_RATE), /* Linear forward bipolar */
	0x0, /* No controller */
	SS_GEN_VIB_LFO_RATE,
	1000,
	0)
};

static size_t INITIAL_MODULATORS_COUNT = sizeof(INITIAL_MODULATORS) / sizeof(INITIAL_MODULATORS[0]);

static void add_modulator(SS_DynamicModulatorSystem *dms, const SS_Modulator *mod) {
	size_t allocated = dms->modulators_allocated;
	if(dms->modulator_count >= allocated) {
		size_t allocated_new = allocated ? allocated * 2 : 8;
		SS_DynamicModulatorSystem_Modulator *new_modulators = realloc(dms->modulators, allocated_new * sizeof(*new_modulators));
		if(new_modulators) {
			dms->modulators = new_modulators;
			dms->modulators_allocated = allocated = allocated_new;
		}
	}
	if(dms->modulator_count < allocated) {
		size_t index = dms->modulator_count++;
		dms->modulators[index].modulator = *mod;
		dms->modulators[index].source = mod->source_enum;
		dms->modulators[index].destination = mod->dest_enum;
		dms->modulators[index].is_bipolar = (mod->source_enum & (1 << 9)) != 0;
		dms->modulators[index].is_negative = (mod->source_enum & (1 << 8)) != 0;
	}
}

void ss_dynamic_modulator_system_init(SS_DynamicModulatorSystem *dms) {
	dms->modulator_count = 0;
	for(size_t i = 0; i < INITIAL_MODULATORS_COUNT; i++) {
		add_modulator(dms, &INITIAL_MODULATORS[i]);
	}
}

void ss_dynamic_modulator_system_free(SS_DynamicModulatorSystem *dms) {
	free(dms->modulators);
	dms->modulators = NULL;
}

static ssize_t find_modulator(SS_DynamicModulatorSystem *dms, uint16_t source,
                              uint16_t destination, bool is_bipolar, bool is_negative) {
	for(size_t i = 0; i < dms->modulator_count; i++) {
		SS_DynamicModulatorSystem_Modulator *mod = &dms->modulators[i];
		if(mod->source == source && mod->destination == destination &&
		   mod->is_bipolar == is_bipolar && mod->is_negative == is_negative) {
			return (ssize_t)i;
		}
	}
	return -1;
}

static void delete_modulator(SS_DynamicModulatorSystem *dms, ssize_t id) {
	if(id < 0) return;
	size_t new_count = 0;
	for(size_t i = 0, _id = (size_t)id, count = dms->modulator_count; i < count; i++) {
		if(i != _id) {
			dms->modulators[new_count++] = dms->modulators[i];
		}
	}
	dms->modulator_count = new_count;
}

static void set_modulator(SS_DynamicModulatorSystem *dms,
                          uint16_t source, uint16_t destination,
                          int16_t amount, bool is_bipolar) {
	ssize_t id = find_modulator(dms, source, destination, is_bipolar, false);
	if(amount == 0) {
		delete_modulator(dms, id);
		return;
	}
	if(id >= 0) {
		SS_DynamicModulatorSystem_Modulator *mod = &dms->modulators[id];
		mod->modulator.transform_amount = amount;
	} else {
		uint16_t srcNum;
		bool isCC;
		if(source >= NON_CC_INDEX_OFFSET) {
			srcNum = source - NON_CC_INDEX_OFFSET;
			isCC = false;
		} else {
			srcNum = source;
			isCC = true;
		}
		SS_Modulator mod = MODULATOR(MODSRC(SS_MODCURVE_LINEAR, false, is_bipolar, isCC, srcNum), 0x0, destination, amount, 0);
		add_modulator(dms, &mod);
	}
}

void ss_dynamic_modulator_system_setup_receiver(SS_DynamicModulatorSystem *dms,
                                                uint8_t addr3, uint8_t data,
                                                uint16_t source, bool is_bipolar) {
	dms->is_active = true;
	const int centeredValue = (int)data - 64;
	const float centeredNormalized = (float)centeredValue / 64.0f;
	const float normalizedNotCentered = (float)data / 127.0f;
	switch(addr3 & 0x0f) {
		case 0x00: {
			/* Pitch control */
			set_modulator(dms, source, SS_GEN_FINE_TUNE, centeredValue * 100, is_bipolar);
			break;
		}

		case 0x01: {
			/* Cutoff */
			set_modulator(dms, source, SS_GEN_INITIAL_FILTER_FC, centeredNormalized * 9600, is_bipolar);
			break;
		}

		case 0x02: {
			/* Amplitude */
			/* Generator is 1/10% */
			set_modulator(dms, source, SS_GEN_AMPLITUDE, centeredNormalized * 1000, is_bipolar);
			break;
		}

		case 0x03: {
			/* LFO1 rate */
			/* Generator is 1/100Hz */
			set_modulator(dms, source, SS_GEN_VIB_LFO_RATE, centeredNormalized * 1000, is_bipolar);
			break;
		}

		case 0x04: {
			set_modulator(dms, source, SS_GEN_VIB_LFO_TO_PITCH, normalizedNotCentered * 600, is_bipolar);
			break;
		}

		case 0x05: {
			/* LFO1 filter depth */
			set_modulator(dms, source, SS_GEN_VIB_LFO_TO_FILTER_FC, normalizedNotCentered * 2400, is_bipolar);
			break;
		}

		case 0x06: {
			/* LFO1 amplitude depth */
			/* Generator is 1/10% */
			set_modulator(dms, source, SS_GEN_VIB_LFO_AMPLITUDE_DEPTH, normalizedNotCentered * 1000, is_bipolar);
			break;
		}

		case 0x07: {
			/* LFO1 rate */
			/* Generator is 1/100Hz */
			set_modulator(dms, source, SS_GEN_MOD_LFO_RATE, centeredNormalized * 1000, is_bipolar);
			break;
		}

		case 0x08: {
			/* LFO2 pitch depth */
			set_modulator(dms, source, SS_GEN_MOD_LFO_TO_PITCH, normalizedNotCentered * 600, is_bipolar);
			break;
		}

		case 0x09: {
			/* LFO2 filter depth */
			set_modulator(dms, source, SS_GEN_MOD_LFO_TO_FILTER_FC, normalizedNotCentered * 2400, is_bipolar);
			break;
		}

		case 0x0a: {
			/* LFO2 amplitude depth */
			/* Generator is 1/10% */
			set_modulator(dms, source, SS_GEN_MOD_LFO_AMPLITUDE_DEPTH, normalizedNotCentered * 1000, is_bipolar);
			break;
		}
	}
}
