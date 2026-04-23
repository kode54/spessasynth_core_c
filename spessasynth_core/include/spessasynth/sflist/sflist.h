#ifndef _SFLIST_H
#define _SFLIST_H

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/file.h>
#include <spessasynth_core/soundbank.h>
#else
#include "spessasynth/utils/file.h"
#include "spessasynth/soundbank/soundbank.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define sflist_max_error 1024



SS_FilteredBanks *sflist_load(const char *sflist, size_t size, const char *base_path, char *error);
void sflist_free(SS_FilteredBanks *);

const char *sflist_upgrade(const char *sflist, size_t size, char *error);
void sflist_upgrade_free(const char *);

#ifdef __cplusplus
}
#endif

#endif
