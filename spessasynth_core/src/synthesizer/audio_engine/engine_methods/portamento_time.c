/**
 * portamento_time.c
 * Portamento time calculation and tables.
 * Port of portamento_time.ts.
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

typedef struct SS_PortamentoLookup {
	uint8_t key;
	float value;
} SS_PortamentoLookup;

static const SS_PortamentoLookup portamento_lookup_table[] = {
	{ 0, 0.0 },
	{ 1, 0.006 },
	{ 2, 0.023 },
	{ 4, 0.05 },
	{ 8, 0.11 },
	{ 16, 0.25 },
	{ 32, 0.5 },
	{ 64, 2.06 },
	{ 80, 4.2 },
	{ 96, 8.4 },
	{ 112, 19.5 },
	{ 116, 26.7 },
	{ 120, 40.0 },
	{ 124, 80.0 },
	{ 127, 480.0 }
};
static const int portamento_lookup_table_count = sizeof(portamento_lookup_table) / sizeof(portamento_lookup_table[0]);

static float ss_portamento_get_lookup(int time) {
	const int count = portamento_lookup_table_count;
	for(int i = 0; i < count; i++) {
		if(portamento_lookup_table[i].key == time)
			return portamento_lookup_table[i].value;
	}

	/* The slow path */
	int lower = -1;
	int lower_index = 0;
	int upper = -1;
	int upper_index = 0;
	for(int i = 0; i < count; i++) {
		int key = portamento_lookup_table[i].key;
		if(key < time && (lower == -1 || key > lower)) {
			lower = key;
			lower_index = i;
		}
		if(key > time && (upper == -1 || key < upper)) {
			upper = key;
			upper_index = i;
		}
	}

	if(lower != -1 && upper != -1) {
		const float lowerTime = portamento_lookup_table[lower_index].value;
		const float upperTime = portamento_lookup_table[upper_index].value;

		return (lowerTime + ((float)(time - lower) * (upperTime - lowerTime)) / (float)(upper - lower));
	}

	return 0;
}

float ss_portamento_time_to_seconds(float portamento_time, float distance) {
	return ss_portamento_get_lookup(portamento_time) * (distance / 36.0);
}
