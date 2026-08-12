/**
 * random.c
 * Seeded random generator.  Port of the splitmix32 in utils/other.ts.
 */

#include <stdint.h>

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/random.h>
#else
#include "spessasynth/utils/random.h"
#endif

/*
 * splitmix32, https://stackoverflow.com/a/47593316
 *
 * The uint32_t arithmetic reproduces the TypeScript bit for bit: `| 0` and
 * `Math.imul` keep JavaScript in wrapping 32-bit integers, `>>>` is a logical
 * shift on the unsigned reinterpretation, and the final `>>> 0` divided by
 * 2^32 is the unsigned value scaled into [0, 1).
 */
double ss_random_next(uint32_t *state) {
	*state += 0x9e3779b9u;
	uint32_t t = *state ^ (*state >> 16);
	t *= 0x21f0aaadu;
	t ^= t >> 15;
	t *= 0x735a2d97u;
	t ^= t >> 15;
	return (double)t / 4294967296.0;
}
