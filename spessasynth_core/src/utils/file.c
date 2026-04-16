//
//  file.c
//  spessasynth_core
//
//  Created by Christopher Snowhill on 4/14/26.
//

#if __has_include(<spessasynth_core/spessasynth.h>)
#include <spessasynth_core/file.h>
#else
#include "spessasynth/utils/file.h"
#endif

#include <stdio.h>

struct SS_File {
	size_t scope_begin, scope_end;
	size_t current_offset;
	SS_Mutex *mutex;
	size_t *ref_count;
	bool owns_file;

	SS_File *(*dup)(SS_File *file);
	void (*close)(SS_File *file);

	uint8_t (*read_u8)(SS_File *file);
	void (*read_bytes)(SS_File *file, uint8_t *out, size_t count);

	bool (*write_u8)(SS_File *file, uint8_t v);
	bool (*write_bytes)(SS_File *file, const uint8_t *src, size_t count);
};

typedef struct SS_FileMemory {
	SS_File base;

	uint8_t *buf;
	size_t allocated;
} SS_FileMemory;

static SS_File *ss_file_memory_dup(SS_File *file) {
	SS_FileMemory *fm = (SS_FileMemory *)file;

	SS_FileMemory *res = (SS_FileMemory *)malloc(sizeof(*res));
	if(!res) return NULL;

	/* Shallow copy */
	memcpy(res, fm, sizeof(*res));

	return &res->base;
}

static void ss_file_memory_close(SS_File *file) {
	SS_FileMemory *fm = (SS_FileMemory *)file;

	if(file->owns_file) {
		free(fm->buf);
	}
}

static uint8_t ss_file_memory_read_u8(SS_File *file) {
	SS_FileMemory *fm = (SS_FileMemory *)file;

	if(file->current_offset < file->scope_end) {
		return fm->buf[file->current_offset++];
	}

	return 0;
}

static void ss_file_memory_read_bytes(SS_File *file, uint8_t *out, size_t count) {
	SS_FileMemory *fm = (SS_FileMemory *)file;

	const size_t max_to_read = file->scope_end - file->current_offset;
	const size_t offset = file->current_offset;
	const size_t to_read = count > max_to_read ? max_to_read : count;

	if(to_read) {
		memcpy(out, fm->buf + offset, to_read);
	}

	file->current_offset += to_read;

	if(to_read < count) {
		memset(out + to_read, 0, count - to_read);
	}
}

static bool ss_file_memory_write_u8(SS_File *file, uint8_t v) {
	SS_FileMemory *fm = (SS_FileMemory *)file;

	if(file->current_offset + 1 < fm->allocated) {
		size_t new_min_size = file->current_offset + 1;
		size_t new_allocated = fm->allocated;
		do {
			new_allocated = new_allocated ? new_allocated * 2 : 1024;
		} while(new_allocated < new_min_size);
		uint8_t *new_buf = (uint8_t *)realloc(fm->buf, new_allocated);
		if(!new_buf) {
			return false;
		}
		fm->buf = new_buf;
		memset(new_buf + fm->allocated, 0, new_allocated - fm->allocated);
		fm->allocated = new_allocated;
	}

	if(file->current_offset < file->scope_end) {
		fm->buf[file->current_offset++] = v;
		return true;
	} else {
		return false;
	}
}

static bool ss_file_memory_write_bytes(SS_File *file, const uint8_t *src, size_t count) {
	SS_FileMemory *fm = (SS_FileMemory *)file;

	if(file->current_offset + count < fm->allocated) {
		size_t new_min_size = file->current_offset + count;
		size_t new_allocated = fm->allocated;
		do {
			new_allocated = new_allocated ? new_allocated * 2 : 1024;
		} while(new_allocated < new_min_size);
		uint8_t *new_buf = (uint8_t *)realloc(fm->buf, new_allocated);
		if(!new_buf) {
			return false;
		}
		fm->buf = new_buf;
		memset(new_buf + fm->allocated, 0, new_allocated - fm->allocated);
		fm->allocated = new_allocated;
	}

	const size_t max_to_write = file->scope_end - file->current_offset;
	const size_t offset = file->current_offset;
	const size_t to_write = count > max_to_write ? max_to_write : count;

	if(to_write) {
		memcpy(fm->buf + offset, src, to_write);
	}

	file->current_offset += to_write;

	return to_write == count;
}

SS_File *ss_file_open_from_memory(const uint8_t *buffer, size_t size, bool owned) {
	SS_FileMemory *res = calloc(1, sizeof(*res));
	if(!res) return NULL;

	res->base.mutex = ss_mutex_create();
	if(!res->base.mutex) {
		free(res);
		return NULL;
	}

	res->base.scope_begin = 0;
	res->base.scope_end = size;
	res->base.current_offset = 0;
	res->base.owns_file = owned;

	res->base.dup = &ss_file_memory_dup;
	res->base.close = &ss_file_memory_close;
	res->base.read_u8 = &ss_file_memory_read_u8;
	res->base.read_bytes = &ss_file_memory_read_bytes;

	res->buf = (uint8_t *)buffer; /* Will not be written to without writer functions */

	return &res->base;
}

SS_File *ss_file_open_blank_memory(void) {
	SS_FileMemory *res = calloc(1, sizeof(*res));
	if(!res) return NULL;

	res->base.mutex = ss_mutex_create();
	if(!res->base.mutex) {
		free(res);
		return NULL;
	}

	res->buf = NULL;
	res->allocated = 0;

	res->base.scope_begin = 0;
	res->base.scope_end = ~0ULL;
	res->base.current_offset = 0;
	res->base.owns_file = true;

	res->base.dup = &ss_file_memory_dup;
	res->base.close = &ss_file_memory_close;
	res->base.write_u8 = &ss_file_memory_write_u8;
	res->base.write_bytes = &ss_file_memory_write_bytes;

	return &res->base;
}

typedef struct SS_FileStdio {
	SS_File base;

	size_t *last_offset;
	FILE *file;
} SS_FileStdio;

static SS_File *ss_file_stdio_dup(SS_File *file) {
	SS_FileStdio *fs = (SS_FileStdio *)file;
	SS_FileStdio *res = (SS_FileStdio *)calloc(1, sizeof(*res));
	if(!res) return NULL;

	/* Shallow copy */
	memcpy(res, fs, sizeof(*res));

	return &res->base;
}

static void ss_file_stdio_close(SS_File *file) {
	SS_FileStdio *fs = (SS_FileStdio *)file;

	if(file->owns_file) {
		fclose(fs->file);
	}

	free(fs->last_offset);
}

static uint8_t ss_file_stdio_read_u8(SS_File *file) {
	SS_FileStdio *fs = (SS_FileStdio *)file;

	if(file->current_offset != *(fs->last_offset)) {
		fseek(fs->file, file->current_offset, SEEK_SET);
		*(fs->last_offset) = ftell(fs->file);
	}

	const size_t max_bytes_to_read = file->scope_end - file->current_offset;
	const size_t to_read = max_bytes_to_read > 0 ? 1 : 0;

	uint8_t v = 0;

	if(to_read) {
		size_t bytes_read = fread(&v, 1, 1, fs->file);

		file->current_offset += bytes_read;
		*(fs->last_offset) = ftell(fs->file);
	}

	return v;
}

static void ss_file_stdio_read_bytes(SS_File *file, uint8_t *out, size_t count) {
	SS_FileStdio *fs = (SS_FileStdio *)file;

	if(file->current_offset != *(fs->last_offset)) {
		fseek(fs->file, file->current_offset, SEEK_SET);
		*(fs->last_offset) = ftell(fs->file);
	}

	const size_t max_bytes_to_read = file->scope_end - file->current_offset;
	const size_t to_read = count > max_bytes_to_read ? max_bytes_to_read : count;
	size_t bytes_read = 0;

	if(to_read) {
		bytes_read = fread(out, 1, to_read, fs->file);

		file->current_offset += bytes_read;
		*(fs->last_offset) = ftell(fs->file);
	}

	if(bytes_read < count) {
		memset(out + bytes_read, 0, count - bytes_read);
	}
}

static bool ss_file_stdio_write_u8(SS_File *file, uint8_t v) {
	SS_FileStdio *fs = (SS_FileStdio *)file;

	const size_t max_bytes_to_write = file->scope_end - file->current_offset;
	const size_t to_write = max_bytes_to_write > 0 ? 1 : 0;

	if(to_write) {
		size_t bytes_written = fwrite(&v, 1, 1, fs->file);

		file->current_offset += bytes_written;

		return !!bytes_written;
	}

	return false;
}

static bool ss_file_stdio_write_bytes(SS_File *file, const uint8_t *src, size_t count) {
	SS_FileStdio *fs = (SS_FileStdio *)file;

	const size_t max_bytes_to_write = file->scope_end - file->current_offset;
	const size_t to_write = count > max_bytes_to_write ? max_bytes_to_write : count;
	size_t bytes_written = 0;

	if(to_write) {
		bytes_written = fwrite(src, 1, to_write, fs->file);

		file->current_offset += bytes_written;
	}

	return bytes_written == count;
}

SS_File *ss_file_open_from_file(const char *path) {
	SS_FileStdio *res = (SS_FileStdio *)calloc(1, sizeof(*res));
	if(!res) return NULL;

	res->base.mutex = ss_mutex_create();
	if(!res->base.mutex) {
		free(res);
		return NULL;
	}

	res->last_offset = calloc(1, sizeof(*(res->last_offset)));
	if(!res->last_offset) {
		ss_mutex_free(res->base.mutex);
		free(res);
		return NULL;
	}

	res->file = fopen(path, "rb");
	if(!res->file) {
		free(res->last_offset);
		ss_mutex_free(res->base.mutex);
		free(res);
		return NULL;
	}

	res->base.scope_begin = 0;
	res->base.current_offset = 0;
	res->base.owns_file = true;

	fseek(res->file, 0, SEEK_END);
	res->base.scope_end = ftell(res->file);
	fseek(res->file, 0, SEEK_SET);
	*(res->last_offset) = ftell(res->file);

	res->base.dup = &ss_file_stdio_dup;
	res->base.close = &ss_file_stdio_close;
	res->base.read_u8 = &ss_file_stdio_read_u8;
	res->base.read_bytes = &ss_file_stdio_read_bytes;

	return &res->base;
}

SS_File *ss_file_open_blank_file(const char *path) {
	SS_FileStdio *res = (SS_FileStdio *)calloc(1, sizeof(*res));
	if(!res) return NULL;

	res->base.mutex = ss_mutex_create();
	if(!res->base.mutex) {
		free(res);
		return NULL;
	}

	res->file = fopen(path, "wb");
	if(!res->file) {
		ss_mutex_free(res->base.mutex);
		free(res);
		return NULL;
	}

	res->base.scope_begin = 0;
	res->base.scope_end = ~0ULL;
	res->base.current_offset = 0;
	res->base.owns_file = true;

	res->base.dup = &ss_file_stdio_dup;
	res->base.close = &ss_file_stdio_close;
	res->base.write_u8 = &ss_file_stdio_write_u8;
	res->base.write_bytes = &ss_file_stdio_write_bytes;

	return &res->base;
}

typedef struct SS_FileCallbacks_Reader {
	SS_File base;

	size_t *last_offset;
	SS_File_ReaderCallbacks callbacks;
	void *context;
} SS_FileCallbacks_Reader;

static SS_File *ss_file_callbacks_reader_dup(SS_File *file) {
	SS_FileCallbacks_Reader *fc = (SS_FileCallbacks_Reader *)file;
	SS_FileCallbacks_Reader *res = (SS_FileCallbacks_Reader *)calloc(1, sizeof(*res));
	if(!res) return NULL;

	/* Shallow copy */
	memcpy(res, fc, sizeof(*res));

	return &res->base;
}

static void ss_file_callbacks_reader_close(SS_File *file) {
	SS_FileCallbacks_Reader *fc = (SS_FileCallbacks_Reader *)file;
	fc->callbacks.close(fc->context);
	free(fc->last_offset);
}

static uint8_t ss_file_callbacks_reader_read_u8(SS_File *file) {
	SS_FileCallbacks_Reader *fc = (SS_FileCallbacks_Reader *)file;

	if(file->current_offset != *(fc->last_offset)) {
		if(!fc->callbacks.seek(fc->context, file->current_offset)) {
			return 0;
		}
		*(fc->last_offset) = file->current_offset;
	}

	const size_t max_bytes_to_read = file->scope_end - file->current_offset;
	const size_t to_read = max_bytes_to_read > 0 ? 1 : 0;

	uint8_t v = 0;

	if(to_read) {
		size_t bytes_read = fc->callbacks.read_bytes(fc->context, &v, 1);

		file->current_offset += bytes_read;
		*(fc->last_offset) += bytes_read;
	}

	return v;
}

static void ss_file_callbacks_reader_read_bytes(SS_File *file, uint8_t *out, size_t count) {
	SS_FileCallbacks_Reader *fc = (SS_FileCallbacks_Reader *)file;

	if(file->current_offset != *(fc->last_offset)) {
		if(!fc->callbacks.seek(fc->context, file->current_offset)) {
			memset(out, 0, count);
			return;
		}
		*(fc->last_offset) = file->current_offset;
	}

	const size_t max_bytes_to_read = file->scope_end - file->current_offset;
	const size_t to_read = count > max_bytes_to_read ? max_bytes_to_read : count;
	size_t bytes_read = 0;

	if(to_read) {
		bytes_read = fc->callbacks.read_bytes(fc->context, out, to_read);

		file->current_offset += bytes_read;
		*(fc->last_offset) += bytes_read;
	}

	if(bytes_read < count) {
		memset(out + bytes_read, 0, count - bytes_read);
	}
}

SS_File *ss_file_open_from_callbacks(SS_File_ReaderCallbacks *callbacks, void *context) {
	if(!callbacks || !callbacks->close || !callbacks->seek || !callbacks->size || !callbacks->read_bytes) {
		return NULL;
	}

	if(!callbacks->seek(context, 0)) {
		return NULL;
	}

	SS_FileCallbacks_Reader *res = (SS_FileCallbacks_Reader *)calloc(1, sizeof(*res));
	if(!res) return NULL;

	res->base.mutex = ss_mutex_create();
	if(!res->base.mutex) {
		free(res);
		return NULL;
	}

	res->last_offset = calloc(1, sizeof(*(res->last_offset)));
	if(!res->last_offset) {
		ss_mutex_free(res->base.mutex);
		free(res);
		return NULL;
	}

	res->callbacks = *callbacks;
	res->context = context;

	res->base.scope_begin = 0;
	res->base.current_offset = 0;
	res->base.owns_file = true;

	res->base.scope_end = callbacks->size(context);
	*(res->last_offset) = 0;

	res->base.dup = &ss_file_callbacks_reader_dup;
	res->base.close = &ss_file_callbacks_reader_close;
	res->base.read_u8 = &ss_file_callbacks_reader_read_u8;
	res->base.read_bytes = &ss_file_callbacks_reader_read_bytes;

	return &res->base;
}

SS_File *ss_file_dup(SS_File *file) {
	ss_mutex_enter(file->mutex);

	if(!file->ref_count) {
		file->ref_count = calloc(1, sizeof(*(file->ref_count)));
		if(!file->ref_count) {
			ss_mutex_leave(file->mutex);
			return NULL;
		}
		*(file->ref_count) = 1;
	}

	SS_File *res = file->dup(file);

	if(res) {
		(*(res->ref_count))++;
	}

	ss_mutex_leave(file->mutex);

	if(!res) {
		return NULL;
	}

	res->current_offset = res->scope_begin;

	return res;
}

SS_File *ss_file_slice(SS_File *file, size_t offset, size_t size) {
	SS_File *res = ss_file_dup(file);
	if(!res) return NULL;

	/* Scope the file */
	size_t new_offset = res->scope_begin + offset;
	if(new_offset > res->scope_end) {
		new_offset = res->scope_end;
	}
	size_t new_scope_end = new_offset + size;
	if(new_scope_end > res->scope_end) {
		new_scope_end = res->scope_end;
	}

	res->scope_begin = new_offset;
	res->current_offset = new_offset;
	res->scope_end = new_scope_end;

	return res;
}

void ss_file_close(SS_File *file) {
	if(!file) return;

	SS_Mutex *mutex = file->mutex;
	ss_mutex_enter(file->mutex);

	bool freed = false;
	if(!file->ref_count || (--(*(file->ref_count))) == 0) {
		file->close(file);
		free(file->ref_count);
		free(file);
		freed = true;
	}

	ss_mutex_leave(mutex);

	if(freed) {
		ss_mutex_free(mutex);
	}
}

size_t ss_file_remaining(SS_File *file) {
	if(!file) return 0;

	ss_mutex_enter(file->mutex);

	size_t res = file->scope_end - file->current_offset;

	ss_mutex_leave(file->mutex);

	return res;
}

uint8_t ss_file_read_u8(SS_File *file, size_t offset) {
	if(!file || !file->read_u8) return 0;
	ss_mutex_enter(file->mutex);
	file->current_offset = file->scope_begin + offset;
	if(file->current_offset > file->scope_end) {
		file->current_offset = file->scope_end;
	}
	uint8_t res = file->read_u8(file);
	ss_mutex_leave(file->mutex);
	return res;
}

size_t ss_file_read_le(SS_File *file, size_t offset, size_t byte_count) {
	if(!file || !file->read_u8) return 0;

	size_t v = 0;

	ss_mutex_enter(file->mutex);

	file->current_offset = file->scope_begin + offset;
	if(file->current_offset > file->scope_end) {
		file->current_offset = file->scope_end;
	}

	for(size_t i = 0; i < byte_count; i++) {
		v |= (size_t)file->read_u8(file) << (8 * i);
	}

	ss_mutex_leave(file->mutex);

	return v;
}

size_t ss_file_read_be(SS_File *file, size_t offset, size_t byte_count) {
	if(!file || !file->read_u8) return 0;

	size_t v = 0;

	ss_mutex_enter(file->mutex);

	file->current_offset = file->scope_begin + offset;
	if(file->current_offset > file->scope_end) {
		file->current_offset = file->scope_end;
	}

	for(size_t i = 0; i < byte_count; i++) {
		v <<= 8;
		v |= (size_t)file->read_u8(file);
	}

	ss_mutex_leave(file->mutex);

	return v;
}

size_t ss_file_read_vlq(SS_File *file, size_t offset) {
	if(!file || !file->read_u8) {
		return 0;
	}

	uint32_t value = 0;
	uint8_t b;

	ss_mutex_enter(file->mutex);

	file->current_offset = file->scope_begin + offset;
	if(file->current_offset > file->scope_end) {
		file->current_offset = file->scope_end;
	}

	do {
		b = file->read_u8(file);
		value = (value << 7) | (b & 0x7F);
	} while(b & 0x80);

	ss_mutex_leave(file->mutex);

	return value;
}

void ss_file_read_bytes(SS_File *file, size_t offset, uint8_t *out, size_t count) {
	if(!file || !file->read_bytes) {
		memset(out, 0, count);
		return;
	}

	ss_mutex_enter(file->mutex);

	file->current_offset = file->scope_begin + offset;
	if(file->current_offset > file->scope_end) {
		file->current_offset = file->scope_end;
	}

	file->read_bytes(file, out, count);

	ss_mutex_leave(file->mutex);
}

void ss_file_read_string(SS_File *file, size_t offset, char *out, size_t count) {
	if(!file || !file->read_bytes) {
		memset(out, 0, count + 1);
		return;
	}

	ss_mutex_enter(file->mutex);

	file->current_offset = file->scope_begin + offset;
	if(file->current_offset > file->scope_end) {
		file->current_offset = file->scope_end;
	}

	file->read_bytes(file, (uint8_t *)out, count);
	out[count] = '\0';

	ss_mutex_leave(file->mutex);
}

bool ss_file_write_u8(SS_File *file, uint8_t v) {
	if(!file || !file->write_u8) {
		return false;
	}

	ss_mutex_enter(file->mutex);

	bool res = file->write_u8(file, v);

	ss_mutex_leave(file->mutex);

	return res;
}

bool ss_file_write_le(SS_File *file, size_t v, size_t byte_count) {
	if(!file || !file->write_u8) {
		return false;
	}

	bool res = true;

	ss_mutex_enter(file->mutex);

	for(size_t i = 0; i < byte_count; i++) {
		res = res && file->write_u8(file, (uint8_t)(v & 0xFF));
		v >>= 8;
	}

	ss_mutex_leave(file->mutex);

	return res;
}

bool ss_file_write_be(SS_File *file, size_t v, size_t byte_count) {
	if(!file || !file->write_u8) {
		return false;
	}

	bool res = true;

	ss_mutex_enter(file->mutex);

	for(size_t i = 0; i < byte_count; i++) {
		res = res && file->write_u8(file, (uint8_t)((v >> (8 * (byte_count - i - 1))) & 0xFF));
	}

	ss_mutex_leave(file->mutex);

	return res;
}

bool ss_file_write_vlq(SS_File *file, size_t v) {
	if(!file || !file->write_u8) {
		return false;
	}

	uint8_t buf[16];
	int n = 0;
	buf[n++] = v & 0x7F;
	v >>= 7;
	while(v) {
		buf[n++] = 0x80 | (v & 0x7F);
		v >>= 7;
	}
	bool res = true;
	ss_mutex_enter(file->mutex);
	for(int i = n - 1; i >= 0; i--) {
		res = res && file->write_u8(file, buf[i]);
	}
	ss_mutex_leave(file->mutex);
	return res;
}

bool ss_file_write_bytes(SS_File *file, const uint8_t *src, size_t count) {
	if(!file || !file->write_bytes) {
		return false;
	}

	ss_mutex_enter(file->mutex);

	bool res = file->write_bytes(file, src, count);

	ss_mutex_leave(file->mutex);

	return res;
}

bool ss_file_write_string(SS_File *file, const char *s, size_t count) {
	if(!file || !file->write_bytes || !file->write_u8) {
		return false;
	}

	bool res = true;

	ss_mutex_enter(file->mutex);

	size_t slen = s ? strlen(s) : 0;
	if(slen) {
		res = file->write_bytes(file, (const uint8_t *)s, slen);
	}

	for(; slen < count; slen++) {
		res = res && file->write_u8(file, 0);
	}

	ss_mutex_leave(file->mutex);

	return res;
}

size_t ss_file_tell(SS_File *file) {
	if(!file) {
		return 0;
	}

	ss_mutex_enter(file->mutex);

	size_t res = file->current_offset - file->scope_begin;

	ss_mutex_leave(file->mutex);

	return res;
}

bool ss_file_retrieve_memory(SS_File *file, uint8_t **out, size_t *out_size) {
	if(!file || !out || !out_size || file->read_u8 || file->read_bytes || file->read_u8 != ss_file_memory_read_u8) {
		return false;
	}

	SS_FileMemory *fm = (SS_FileMemory *)file;
	*out = fm->buf;
	*out_size = file->current_offset;

	return true;
}

void ss_file_skip(SS_File *file, size_t skip) {
	if(!file) {
		return;
	}

	ss_mutex_enter(file->mutex);

	file->current_offset += skip;
	if(file->current_offset > file->scope_end) {
		file->current_offset = file->scope_end;
	}

	ss_mutex_leave(file->mutex);
}

size_t ss_file_size(SS_File *file) {
	if(!file) return 0;

	ss_mutex_enter(file->mutex);

	size_t res = file->scope_end - file->scope_begin;

	ss_mutex_leave(file->mutex);

	return res;
}

void ss_file_seek(SS_File *file, size_t offset) {
	if(!file) return;

	ss_mutex_enter(file->mutex);

	file->current_offset = file->scope_begin + offset;

	ss_mutex_leave(file->mutex);
}
