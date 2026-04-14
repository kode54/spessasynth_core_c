//
//  sync_primitives.h
//  spessasynth_core
//
//  Created by Christopher Snowhill on 4/14/26.
//

#ifndef SS_SYNC_PRIMITIVES_H
#define SS_SYNC_PRIMITIVES_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SS_Mutex SS_Mutex;

SS_Mutex *ss_mutex_create(void);
void ss_mutex_free(SS_Mutex *mutex);

void ss_mutex_enter(SS_Mutex *mutex);
void ss_mutex_leave(SS_Mutex *mutex);

#ifdef __cplusplus
}
#endif

#endif /* SS_SYNC_PRIMITIVES_H */
