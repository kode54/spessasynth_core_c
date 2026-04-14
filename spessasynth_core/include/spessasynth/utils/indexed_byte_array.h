#ifndef SS_INDEXED_BYTE_ARRAY_H
#define SS_INDEXED_BYTE_ARRAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * IndexedByteArray — equivalent of the TypeScript IndexedByteArray class.
 * A byte buffer with a current read/write cursor (currentIndex).
 *
 * Ownership: if owns_data is true, ss_iba_free() will free data.
 */
typedef struct {
	uint8_t *data;
	size_t length;
	size_t current_index;
	bool owns_data;
} SS_IBA;

/** Wrap an existing buffer (no ownership transfer). */
static inline void ss_iba_wrap(SS_IBA *iba, const uint8_t *data, size_t length) {
	iba->data = (uint8_t *)data;
	iba->length = length;
	iba->current_index = 0;
	iba->owns_data = false;
}

/** Allocate a zeroed writable buffer (IBA takes ownership). */
static inline bool ss_iba_alloc(SS_IBA *iba, size_t length) {
	void *p = calloc(1, length > 0 ? length : 1);
	if(!p) return false;
	iba->data = (uint8_t *)p;
	iba->length = length;
	iba->current_index = 0;
	iba->owns_data = true;
	return true;
}

/**
 * Slice: allocates a copy of src->data[start..end) and wraps it.
 * The new IBA owns its data; current_index is reset to 0.
 */
static inline bool ss_iba_slice(const SS_IBA *src, size_t start, size_t end, SS_IBA *out) {
	if(end > src->length) end = src->length;
	size_t len = (end > start) ? (end - start) : 0;
	if(!ss_iba_alloc(out, len)) return false;
	if(len > 0) memcpy(out->data, src->data + start, len);
	return true;
}

/** Free owned data. Safe to call on non-owning IBAs (no-op). */
static inline void ss_iba_free(SS_IBA *iba) {
	if(iba->owns_data && iba->data) {
		free(iba->data);
		iba->data = NULL;
		iba->owns_data = false;
	}
	iba->length = 0;
	iba->current_index = 0;
}

/** Bytes remaining from current_index to end. */
static inline size_t ss_iba_remaining(const SS_IBA *iba) {
	return (iba->current_index <= iba->length) ? (iba->length - iba->current_index) : 0;
}

/** Read a single byte; advances current_index. Returns 0 on underflow. */
static inline uint8_t ss_iba_read_u8(SS_IBA *iba) {
	if(iba->current_index >= iba->length) return 0;
	return iba->data[iba->current_index++];
}

/** Read little-endian unsigned value (1-4 bytes); advances current_index. */
static inline size_t ss_iba_read_le(SS_IBA *iba, int byte_count) {
	size_t out = 0;
	for(int i = 0; i < byte_count; i++) {
		out |= (size_t)ss_iba_read_u8(iba) << (i * 8);
	}
	return out;
}

/** Read big-endian unsigned value (1-4 bytes); advances current_index. */
static inline size_t ss_iba_read_be(SS_IBA *iba, int byte_count) {
	size_t out = 0;
	for(int i = 0; i < byte_count; i++) {
		out = (out << 8) | ss_iba_read_u8(iba);
	}
	return out;
}

/** Read a MIDI variable-length quantity; advances current_index. */
static inline size_t ss_iba_read_vlq(SS_IBA *iba) {
	size_t value = 0;
	uint8_t b;
	do {
		b = ss_iba_read_u8(iba);
		value = (value << 7) | (b & 0x7F);
	} while(b & 0x80);
	return value;
}

/** Read count bytes into out; advances current_index. */
static inline void ss_iba_read_bytes(SS_IBA *iba, uint8_t *out, size_t count) {
	for(size_t i = 0; i < count; i++) {
		out[i] = ss_iba_read_u8(iba);
	}
}

/**
 * Read exactly count bytes as a C string (NUL-terminates out[count]).
 * out must be at least count+1 bytes.
 */
static inline void ss_iba_read_string(SS_IBA *iba, char *out, size_t count) {
	ss_iba_read_bytes(iba, (uint8_t *)out, count);
	out[count] = '\0';
}

/** Write a single byte; advances current_index. */
static inline void ss_iba_write_u8(SS_IBA *iba, uint8_t v) {
	if(iba->current_index < iba->length)
		iba->data[iba->current_index++] = v;
}

/** Write little-endian value (byte_count bytes); advances current_index. */
static inline void ss_iba_write_le(SS_IBA *iba, size_t v, int byte_count) {
	for(int i = 0; i < byte_count; i++) {
		ss_iba_write_u8(iba, (uint8_t)(v >> (i * 8)));
	}
}

/** Write big-endian value (byte_count bytes); advances current_index. */
static inline void ss_iba_write_be(SS_IBA *iba, size_t v, int byte_count) {
	for(int i = byte_count - 1; i >= 0; i--) {
		if(iba->current_index + (size_t)(byte_count - 1 - i) < iba->length)
			iba->data[iba->current_index++] = (uint8_t)(v >> (i * 8));
	}
}

/** Write a MIDI variable-length quantity; advances current_index. */
static inline void ss_iba_write_vlq(SS_IBA *iba, size_t v) {
	uint8_t buf[16];
	int n = 0;
	buf[n++] = v & 0x7F;
	v >>= 7;
	while(v) {
		buf[n++] = 0x80 | (v & 0x7F);
		v >>= 7;
	}
	for(int i = n - 1; i >= 0; i--) {
		ss_iba_write_u8(iba, buf[i]);
	}
}

/** Write count bytes from src into iba at current_index; advances current_index. */
static inline void ss_iba_write_bytes(SS_IBA *iba, const uint8_t *src, size_t count) {
	for(size_t i = 0; i < count; i++) {
		ss_iba_write_u8(iba, src[i]);
	}
}

/** Write a C string (without NUL) of exactly count bytes, space-padded. */
static inline void ss_iba_write_string(SS_IBA *iba, const char *s, size_t count) {
	size_t slen = s ? strlen(s) : 0;
	for(size_t i = 0; i < count; i++) {
		ss_iba_write_u8(iba, (i < slen) ? (uint8_t)s[i] : 0);
	}
}

#ifdef __cplusplus
}
#endif

#endif /* SS_INDEXED_BYTE_ARRAY_H */
