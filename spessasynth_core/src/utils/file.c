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

#ifdef _WIN32
#include <windows.h>

#include <wchar.h>

/* Open a file whose path is UTF-8 encoded.  Windows fopen takes the
 * system ANSI codepage which can't represent arbitrary Unicode paths,
 * so convert UTF-8 → UTF-16 and use _wfopen.  A stack buffer sized for
 * MAX_PATH is used when possible; longer paths fall back to malloc. */
static FILE *ss_fopen_utf8(const char *path, const char *mode) {
	wchar_t stack_path[320]; /* 260-char MAX_PATH plus headroom */
	wchar_t wmode[8];
	int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
	if(wlen <= 0) return NULL;

	wchar_t *wpath;
	wchar_t *heap_path = NULL;
	if((size_t)wlen <= sizeof(stack_path) / sizeof(stack_path[0])) {
		wpath = stack_path;
	} else {
		heap_path = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
		if(!heap_path) return NULL;
		wpath = heap_path;
	}
	if(MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen) <= 0) {
		free(heap_path);
		return NULL;
	}

	if(MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode,
	                       sizeof(wmode) / sizeof(wmode[0])) <= 0) {
		free(heap_path);
		return NULL;
	}

	FILE *f = _wfopen(wpath, wmode);
	free(heap_path);
	return f;
}
#else
#define ss_fopen_utf8(path, mode) fopen((path), (mode))
#endif

struct SS_File {
	uint64_t scope_begin, scope_end;
	uint64_t current_offset;
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

	const uint64_t max_to_read = file->scope_end - file->current_offset;
	const uint64_t offset = file->current_offset;
	const uint64_t to_read = count > max_to_read ? max_to_read : count;

	if(to_read) {
		memcpy(out, fm->buf + offset, (size_t)to_read);
	}

	file->current_offset += to_read;

	if(to_read < count) {
		memset(out + to_read, 0, (size_t)(count - to_read));
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

/* Read-ahead buffer size used by the SS_FileStdio reader.  Override at
 * compile time with -DSS_FILESTDIO_BUFFER_SIZE=N. */
#ifndef SS_FILESTDIO_BUFFER_SIZE
#define SS_FILESTDIO_BUFFER_SIZE 16384
#endif

typedef struct SS_FileStdio {
	SS_File base;

	size_t *last_offset;
	FILE *file;

	/* Read-ahead buffer.  All three pointers are heap-allocated by
	 * ss_file_open_from_file and freed by ss_file_stdio_close.  Slicing
	 * via ss_file_dup performs a shallow copy of the SS_FileStdio struct,
	 * so the buffer (and its start/filled state) are shared across every
	 * SS_File handle that points at the same underlying FILE. */
	uint8_t *buffer;
	size_t *buffer_start;  /* Start offset (in the FILE) of the current buffered range */
	size_t *buffer_filled; /* Number of valid bytes currently held in buffer */
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
	free(fs->buffer);
	free(fs->buffer_start);
	free(fs->buffer_filled);
}

/* Refill the read-ahead buffer starting at file offset `target`, then
 * update *last_offset so the next physical read in the same place is a
 * no-op.  Stores the actual byte count in *buffer_filled — when reading
 * near EOF, this may be less than SS_FILESTDIO_BUFFER_SIZE (or zero). */
static void ss_file_stdio_buffer_fill(SS_FileStdio *fs, size_t target) {
	if(*(fs->last_offset) != target) {
		fseek(fs->file, (long)target, SEEK_SET);
	}
	size_t got = fread(fs->buffer, 1, SS_FILESTDIO_BUFFER_SIZE, fs->file);
	*(fs->buffer_start) = target;
	*(fs->buffer_filled) = got;
	*(fs->last_offset) = target + got;
}

static uint8_t ss_file_stdio_read_u8(SS_File *file) {
	SS_FileStdio *fs = (SS_FileStdio *)file;

	if(file->current_offset >= file->scope_end) return 0;

	size_t off = file->current_offset;
	size_t buf_start = *(fs->buffer_start);
	size_t buf_filled = *(fs->buffer_filled);

	if(off < buf_start || off >= buf_start + buf_filled) {
		ss_file_stdio_buffer_fill(fs, off);
		buf_start = *(fs->buffer_start);
		buf_filled = *(fs->buffer_filled);
		if(buf_filled == 0) return 0;
	}

	uint8_t v = fs->buffer[off - buf_start];
	file->current_offset++;
	return v;
}

static void ss_file_stdio_read_bytes(SS_File *file, uint8_t *out, size_t count) {
	SS_FileStdio *fs = (SS_FileStdio *)file;

	size_t to_read = 0;
	if(file->current_offset < file->scope_end) {
		const size_t max_bytes_to_read = file->scope_end - file->current_offset;
		to_read = count > max_bytes_to_read ? max_bytes_to_read : count;
	}

	size_t copied = 0;
	while(copied < to_read) {
		size_t off = file->current_offset;
		size_t buf_start = *(fs->buffer_start);
		size_t buf_filled = *(fs->buffer_filled);

		if(off < buf_start || off >= buf_start + buf_filled) {
			ss_file_stdio_buffer_fill(fs, off);
			buf_start = *(fs->buffer_start);
			buf_filled = *(fs->buffer_filled);
			if(buf_filled == 0) break; /* EOF */
		}

		size_t in_buf = (buf_start + buf_filled) - off;
		size_t n = (to_read - copied < in_buf) ? to_read - copied : in_buf;
		memcpy(out + copied, fs->buffer + (off - buf_start), n);
		copied += n;
		file->current_offset += n;
	}

	if(copied < count) {
		memset(out + copied, 0, count - copied);
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
	res->buffer = (uint8_t *)malloc(SS_FILESTDIO_BUFFER_SIZE);
	res->buffer_start = calloc(1, sizeof(*(res->buffer_start)));
	res->buffer_filled = calloc(1, sizeof(*(res->buffer_filled)));
	if(!res->last_offset || !res->buffer || !res->buffer_start || !res->buffer_filled) {
		free(res->last_offset);
		free(res->buffer);
		free(res->buffer_start);
		free(res->buffer_filled);
		ss_mutex_free(res->base.mutex);
		free(res);
		return NULL;
	}

	res->file = ss_fopen_utf8(path, "rb");
	if(!res->file) {
		free(res->last_offset);
		free(res->buffer);
		free(res->buffer_start);
		free(res->buffer_filled);
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

	/* Pre-read the head of the file so the first access is served from
	 * the buffer.  Subsequent ranges are loaded on demand whenever a read
	 * crosses outside the currently buffered window. */
	ss_file_stdio_buffer_fill(res, 0);

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

	res->file = ss_fopen_utf8(path, "wb");
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
		freed = true;
	}

	/* Files are duped to new memory, so each one must be freed */
	free(file);

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

uint16_t ss_file_read_le16(SS_File *file, size_t offset) {
	if(!file || !file->read_u8) return 0;

	ss_mutex_enter(file->mutex);

	file->current_offset = file->scope_begin + offset;
	if(file->current_offset > file->scope_end) {
		file->current_offset = file->scope_end;
	}

	uint16_t v = file->read_u8(file);
	v |= (uint16_t)file->read_u8(file) << 8;

	ss_mutex_leave(file->mutex);

	return v;
}

uint32_t ss_file_read_le24(SS_File *file, size_t offset) {
	if(!file || !file->read_u8) return 0;

	ss_mutex_enter(file->mutex);

	file->current_offset = file->scope_begin + offset;
	if(file->current_offset > file->scope_end) {
		file->current_offset = file->scope_end;
	}

	uint32_t v = file->read_u8(file);
	v |= (uint32_t)file->read_u8(file) << 8;
	v |= (uint32_t)file->read_u8(file) << 16;

	ss_mutex_leave(file->mutex);

	return v;
}

uint32_t ss_file_read_le32(SS_File *file, size_t offset) {
	if(!file || !file->read_u8) return 0;

	ss_mutex_enter(file->mutex);

	file->current_offset = file->scope_begin + offset;
	if(file->current_offset > file->scope_end) {
		file->current_offset = file->scope_end;
	}

	uint32_t v = file->read_u8(file);
	v |= (uint32_t)file->read_u8(file) << 8;
	v |= (uint32_t)file->read_u8(file) << 16;
	v |= (uint32_t)file->read_u8(file) << 24;

	ss_mutex_leave(file->mutex);

	return v;
}

uint16_t ss_file_read_be16(SS_File *file, size_t offset) {
	if(!file || !file->read_u8) return 0;

	ss_mutex_enter(file->mutex);

	file->current_offset = file->scope_begin + offset;
	if(file->current_offset > file->scope_end) {
		file->current_offset = file->scope_end;
	}

	uint16_t v = (uint16_t)file->read_u8(file) << 8;
	v |= (uint16_t)file->read_u8(file);

	ss_mutex_leave(file->mutex);

	return v;
}

uint32_t ss_file_read_be24(SS_File *file, size_t offset) {
	if(!file || !file->read_u8) return 0;

	ss_mutex_enter(file->mutex);

	file->current_offset = file->scope_begin + offset;
	if(file->current_offset > file->scope_end) {
		file->current_offset = file->scope_end;
	}

	uint32_t v = (uint32_t)file->read_u8(file) << 16;
	v |= (uint32_t)file->read_u8(file) << 8;
	v |= (uint32_t)file->read_u8(file);

	ss_mutex_leave(file->mutex);

	return v;
}

uint32_t ss_file_read_be32(SS_File *file, size_t offset) {
	if(!file || !file->read_u8) return 0;

	ss_mutex_enter(file->mutex);

	file->current_offset = file->scope_begin + offset;
	if(file->current_offset > file->scope_end) {
		file->current_offset = file->scope_end;
	}

	uint32_t v = (uint32_t)file->read_u8(file) << 24;
	v |= (uint32_t)file->read_u8(file) << 16;
	v |= (uint32_t)file->read_u8(file) << 8;
	v |= (uint32_t)file->read_u8(file);

	ss_mutex_leave(file->mutex);

	return v;
}