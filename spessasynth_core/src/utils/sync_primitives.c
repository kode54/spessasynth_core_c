//
//  sync_primitives.c
//  spessasynth_core
//
//  Created by Christopher Snowhill on 4/14/26.
//

#include <string.h>
#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/sync_primitives.h>
#else
#include "spessasynth/utils/sync_primitives.h"
#endif

#if defined(__linux__) || defined(__APPLE__)

/**
 * pthread based mutex implementation.
 *
 * Intentionally the default mutex, non-recursive.
 *
 * Feel free to contribute other platform detection macros where pthread is supplied!
 */

#include <pthread.h>
#include <stdlib.h>

struct SS_Mutex {
	pthread_mutex_t mutex;
};

SS_Mutex *ss_mutex_create(void) {
	SS_Mutex *res = calloc(1, sizeof(*res));
	if(!res) return NULL;

	if(pthread_mutex_init(&res->mutex, NULL) != 0) {
		free(res);
		return NULL;
	}

	return res;
}

void ss_mutex_free(SS_Mutex *mutex) {
	if(!mutex) return;

	pthread_mutex_destroy(&mutex->mutex);

	free(mutex);
}

void ss_mutex_enter(SS_Mutex *mutex) {
	if(!mutex) return;

	pthread_mutex_lock(&mutex->mutex);
}

void ss_mutex_leave(SS_Mutex *mutex) {
	if(!mutex) return;

	pthread_mutex_unlock(&mutex->mutex);
}

#elif defined(_WIN32)

/**
 * The basest Windows implementation.
 *
 * Possibly recursive. Not by intention. Doesn't need to be for our uses.
 */

#include <stdlib.h>
#include <windows.h>

struct SS_Mutex {
	CRITICAL_SECTION mutex;
};

SS_Mutex *ss_mutex_create(void) {
	SS_Mutex *res = calloc(1, sizeof(*res));
	if(!res) return NULL;

	InitializeCriticalSection(&res->mutex);

	return res;
}

void ss_mutex_free(SS_Mutex *mutex) {
	if(!mutex) return;

	DeleteCriticalSection(&mutex->mutex);

	free(mutex);
}

void ss_mutex_enter(SS_Mutex *mutex) {
	if(!mutex) return;

	EnterCriticalSection(&mutex->mutex);
}

void ss_mutex_leave(SS_Mutex *mutex) {
	if(!mutex) return;

	LeaveCriticalSection(&mutex->mutex);
}

#else
/**
 * No detected implementation found.
 *
 * Feel free to help out by contributing your own, or by contributing to the
 * macros used to detect the pthread implementation.
 */
#error No Mutex implementation! Please feel free to contribute one!
#endif
