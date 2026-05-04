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

#ifdef _MSC_VER
#include "spessasynth_exports.h"
#else
#define SPESSASYNTH_EXPORTS
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SS_File SS_File;

/*
 The file primitive is fully reference counted, and will be freely duplicated or
 sliced by the file loaders. Since any SS_SoundBank will keep handles to a file
 after it is opened, it is safe to close the handle yourself immediately after
 opening the bank, whether or not it successfully loads a bank. If the memory
 reader was called without handing over ownership of the memory buffer, then it
 is essential to keep that buffer somewhere for the lifetime of the file, so it
 will be necessary to release it only after freeing any SS_SoundBanks which own it.
 */

SS_File SPESSASYNTH_EXPORTS *ss_file_open_from_memory(const uint8_t *buffer, size_t size, bool owned);
SS_File SPESSASYNTH_EXPORTS *ss_file_open_from_file(const char *path);

/*
 The following callbacks will be guaranteed to be synchronized to only happen
 on one thread at a time.

 size() will only be called on startup, in the same thread as the opener.

 seek() will be called if the file offset needs to be changed within the
 current thread, and will be hopped around if other threads are also calling
 the file object. Note that due to the synchronization, seek()/read_bytes()
 will be serialized together, or not at all, if the offset hasn't changed.

 read_bytes() will always read from the last offset, and return the number
 of bytes read, or otherwise zero if nothing was read. The caller will take
 care of filling the rest of the requested buffer, or the whole buffer on
 error.

 close() will be called when every reference counted instance of the main
 file is closed. So, it will be safe to leave your objects to be owned by
 your context data, and deleted or freed by the close callback.
 */
typedef struct SS_File_ReaderCallbacks {
	void (*close)(void *context);
	bool (*seek)(void *context, size_t offset);
	size_t (*size)(void *context);
	size_t (*read_bytes)(void *context, uint8_t *out, size_t count);
} SS_File_ReaderCallbacks;

SS_File SPESSASYNTH_EXPORTS *ss_file_open_from_callbacks(SS_File_ReaderCallbacks *callbacks, void *context);

SS_File SPESSASYNTH_EXPORTS *ss_file_open_blank_memory(void);
bool SPESSASYNTH_EXPORTS ss_file_retrieve_memory(SS_File *file, uint8_t **out, size_t *out_size);

SS_File SPESSASYNTH_EXPORTS *ss_file_open_blank_file(const char *path);

SS_File SPESSASYNTH_EXPORTS *ss_file_dup(SS_File *file);

SS_File SPESSASYNTH_EXPORTS *ss_file_slice(SS_File *file, size_t offset, size_t size);

void SPESSASYNTH_EXPORTS ss_file_close(SS_File *file);

size_t SPESSASYNTH_EXPORTS ss_file_remaining(SS_File *file);
size_t SPESSASYNTH_EXPORTS ss_file_size(SS_File *file);
void SPESSASYNTH_EXPORTS ss_file_seek(SS_File *file, size_t offset);
size_t SPESSASYNTH_EXPORTS ss_file_tell(SS_File *file);
void SPESSASYNTH_EXPORTS ss_file_skip(SS_File *file, size_t skip);

uint8_t SPESSASYNTH_EXPORTS ss_file_read_u8(SS_File *file, size_t offset);
size_t SPESSASYNTH_EXPORTS ss_file_read_le(SS_File *file, size_t offset, size_t byte_count);
size_t SPESSASYNTH_EXPORTS ss_file_read_be(SS_File *file, size_t offset, size_t byte_count);
size_t SPESSASYNTH_EXPORTS ss_file_read_vlq(SS_File *file, size_t offset);
void SPESSASYNTH_EXPORTS ss_file_read_bytes(SS_File *file, size_t offset, uint8_t *out, size_t count);
void SPESSASYNTH_EXPORTS ss_file_read_string(SS_File *file, size_t offset, char *out, size_t count);

bool SPESSASYNTH_EXPORTS ss_file_write_u8(SS_File *file, uint8_t v);
bool SPESSASYNTH_EXPORTS ss_file_write_le(SS_File *file, size_t v, size_t byte_count);
bool SPESSASYNTH_EXPORTS ss_file_write_be(SS_File *file, size_t v, size_t byte_count);
bool SPESSASYNTH_EXPORTS ss_file_write_vlq(SS_File *file, size_t v);
bool SPESSASYNTH_EXPORTS ss_file_write_bytes(SS_File *file, const uint8_t *src, size_t count);
bool SPESSASYNTH_EXPORTS ss_file_write_string(SS_File *file, const char *s, size_t count);

/* Static size helpers, should be faster */
uint16_t SPESSASYNTH_EXPORTS ss_file_read_le16(SS_File *file, size_t offset);
uint32_t SPESSASYNTH_EXPORTS ss_file_read_le24(SS_File *file, size_t offset);
uint32_t SPESSASYNTH_EXPORTS ss_file_read_le32(SS_File *file, size_t offset);

uint16_t SPESSASYNTH_EXPORTS ss_file_read_be16(SS_File *file, size_t offset);
uint32_t SPESSASYNTH_EXPORTS ss_file_read_be24(SS_File *file, size_t offset);
uint32_t SPESSASYNTH_EXPORTS ss_file_read_be32(SS_File *file, size_t offset);

#ifdef __cplusplus
}
#endif

#endif /* SS_RIFF_CHUNK_H */
