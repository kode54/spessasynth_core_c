//
//  sync_primitives.h
//  spessasynth_core
//
//  Created by Christopher Snowhill on 4/14/26.
//

#ifndef SS_SYNC_PRIMITIVES_H
#define SS_SYNC_PRIMITIVES_H

#ifdef _MSC_VER
#include "spessasynth_exports.h"
#else
#define SPESSASYNTH_EXPORTS
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque structure, depends on the implementation.
 *
 * Does not need to be recursive, as it will never be used recursively by the player.
 *
 * May actually be recursive in the Windows implementation.
 */

typedef struct SS_Mutex SS_Mutex;

SS_Mutex SPESSASYNTH_EXPORTS *ss_mutex_create(void);
void SPESSASYNTH_EXPORTS ss_mutex_free(SS_Mutex *mutex);

void SPESSASYNTH_EXPORTS ss_mutex_enter(SS_Mutex *mutex);
void SPESSASYNTH_EXPORTS ss_mutex_leave(SS_Mutex *mutex);

#ifdef __cplusplus
}
#endif

#endif /* SS_SYNC_PRIMITIVES_H */
