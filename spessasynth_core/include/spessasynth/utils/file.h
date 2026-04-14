//
//  file.h
//  spessasynth_core
//
//  Created by Christopher Snowhill on 4/14/26.
//

#ifndef SS_FILE_H
#define SS_FILE_H

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/sync_primitives.h>
#else
#include "sync_primitives.h"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SS_File SS_File;

SS_File *ss_file_open_from_memory(const uint8_t *buffer, size_t size, bool owned);
SS_File *ss_file_open_from_file(const char *path);

SS_File *ss_file_open_blank_memory(void);
bool ss_file_retrieve_memory(SS_File *file, uint8_t **out, size_t *out_size);

SS_File *ss_file_open_blank_file(const char *path);

SS_File *ss_file_dup(SS_File *file);

SS_File *ss_file_slice(SS_File *file, size_t offset, size_t size);

void ss_file_close(SS_File *file);

size_t ss_file_remaining(SS_File *file);
size_t ss_file_size(SS_File *file);
void ss_file_seek(SS_File *file, size_t offset);
size_t ss_file_tell(SS_File *file);
void ss_file_skip(SS_File *file, size_t skip);

uint8_t ss_file_read_u8(SS_File *file, size_t offset);
size_t ss_file_read_le(SS_File *file, size_t offset, size_t byte_count);
size_t ss_file_read_be(SS_File *file, size_t offset, size_t byte_count);
size_t ss_file_read_vlq(SS_File *file, size_t offset);
void ss_file_read_bytes(SS_File *file, size_t offset, uint8_t *out, size_t count);
void ss_file_read_string(SS_File *file, size_t offset, char *out, size_t count);

bool ss_file_write_u8(SS_File *file, uint8_t v);
bool ss_file_write_le(SS_File *file, size_t v, size_t byte_count);
bool ss_file_write_be(SS_File *file, size_t v, size_t byte_count);
bool ss_file_write_vlq(SS_File *file, size_t v);
bool ss_file_write_bytes(SS_File *file, const uint8_t *src, size_t count);
bool ss_file_write_string(SS_File *file, const char *s, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* SS_RIFF_CHUNK_H */
