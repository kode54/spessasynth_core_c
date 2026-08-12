#ifndef SS_RANDOM_H
#define SS_RANDOM_H

#include <stdint.h>

#ifdef _MSC_VER
#include "spessasynth_exports.h"
#else
#define SPESSASYNTH_EXPORTS
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Seed upstream's generator starts from.  A fixed seed is the point:
 * random panning has to fall the same way on every run, or a render cannot be
 * compared with itself, let alone with another engine.
 */
#define SS_RANDOM_DEFAULT_SEED 81572u

/**
 * splitmix32, stepping a caller-owned state.
 *
 * The state lives with whoever owns the sequence — SS_Processor keeps one, so
 * two processors in a program draw independently and each is reproducible on
 * its own. Upstream's equivalent is a module-level singleton; keeping the
 * state explicit here costs nothing and avoids one render's panning depending
 * on how many notes some other synthesizer happened to play first.
 *
 * @param state stepped in place; seed it with SS_RANDOM_DEFAULT_SEED.
 * @returns the next value in [0, 1).
 */
double SPESSASYNTH_EXPORTS ss_random_next(uint32_t *state);

#ifdef __cplusplus
}
#endif

#endif /* SS_RANDOM_H */
