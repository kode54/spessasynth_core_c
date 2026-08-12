/**
 * insertion_list.c
 * Registry of the available insertion effects.
 *
 * Port of upstream effects/insertion_list.ts, which holds the equivalent
 * array of processor constructors.  Each entry lives in its own file under
 * insertion/.
 */

#include "insertion/insertion_internal.h"

typedef struct {
	uint32_t type; /* MSB<<8 | LSB */
	SS_InsertionProcessor *(*create)(uint32_t type, uint32_t sample_rate,
	                                 uint32_t max_buf_size);
} SS_InsertionEntry;

static const SS_InsertionEntry INSERTION_EFFECT_LIST[] = {
	{ 0x0000, ss_insertion_thru_create },
	{ 0x0100, ss_insertion_stereo_eq_create },
	{ 0x0120, ss_insertion_phaser_create },
	{ 0x0121, ss_insertion_auto_wah_create },
	{ 0x0125, ss_insertion_tremolo_create },
	{ 0x0126, ss_insertion_auto_pan_create },
	{ 0x1108, ss_insertion_ph_auto_wah_create }
};

static const size_t INSERTION_EFFECT_COUNT =
sizeof(INSERTION_EFFECT_LIST) / sizeof(INSERTION_EFFECT_LIST[0]);

SS_InsertionProcessor *ss_insertion_create(uint32_t type,
                                           uint32_t sample_rate,
                                           uint32_t max_buf_size) {
	ss_init_insertion_pan_tables();

	for(size_t i = 0; i < INSERTION_EFFECT_COUNT; i++) {
		if(INSERTION_EFFECT_LIST[i].type == type) {
			return INSERTION_EFFECT_LIST[i].create(type, sample_rate,
			                                       max_buf_size);
		}
	}
	return NULL;
}
