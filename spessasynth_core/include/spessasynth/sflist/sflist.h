#ifndef _SFLIST_H
#define _SFLIST_H

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/file.h>
#include <spessasynth_core/soundbank.h>
#else
#include "spessasynth/soundbank/soundbank.h"
#include "spessasynth/utils/file.h"
#endif

#ifdef _MSC_VER
#include "spessasynth_exports.h"
#else
#define SPESSASYNTH_EXPORTS
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define sflist_max_error 1024

typedef SS_File* (*sflist_open_callback)(void *context, const char *path);

SS_FilteredBanks SPESSASYNTH_EXPORTS *sflist_load(const char *sflist, size_t size, const char *base_path, char *error);
SS_FilteredBanks SPESSASYNTH_EXPORTS *sflist_load_callback(const char *sflist, size_t size, const char *base_path, char *error, sflist_open_callback font_open, void *open_context);
void SPESSASYNTH_EXPORTS sflist_free(SS_FilteredBanks *);

const char SPESSASYNTH_EXPORTS *sflist_upgrade(const char *sflist, size_t size, char *error);
void SPESSASYNTH_EXPORTS sflist_upgrade_free(const char *);

#ifdef __cplusplus
}
#endif

#endif
