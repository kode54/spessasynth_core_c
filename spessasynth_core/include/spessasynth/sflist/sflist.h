#ifndef _SFLIST_H
#define _SFLIST_H

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/file.h>
#include <spessasynth_core/soundbank.h>
#else
#include "spessasynth/soundbank/soundbank.h"
#include "spessasynth/utils/file.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define sflist_max_error 1024

typedef SS_File* (*sflist_open_callback)(void *context, const char *path);

SS_FilteredBanks *sflist_load(const char *sflist, size_t size, const char *base_path, char *error);
SS_FilteredBanks *sflist_load_callback(const char *sflist, size_t size, const char *base_path, char *error, sflist_open_callback font_open, void *open_context);
void sflist_free(SS_FilteredBanks *);

const char *sflist_upgrade(const char *sflist, size_t size, char *error);
void sflist_upgrade_free(const char *);

#ifdef __cplusplus
}
#endif

#endif
