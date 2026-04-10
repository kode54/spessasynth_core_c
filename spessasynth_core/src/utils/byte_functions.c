/**
 * byte_functions.c
 * Byte utility functions (little/big-endian, VLQ, string helpers).
 * Most functionality lives inline in indexed_byte_array.h;
 * this file provides any non-inline implementations.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * Read a null-terminated or length-bounded ASCII string from buf into dst.
 * Returns number of bytes consumed (not including trailing null if present).
 */
size_t ss_read_ascii(const uint8_t *buf, size_t buf_len,
                     char *dst, size_t dst_size) {
	size_t i = 0;
	size_t limit = (buf_len < dst_size - 1) ? buf_len : dst_size - 1;
	while(i < limit && buf[i] != '\0') {
		dst[i] = (char)buf[i];
		i++;
	}
	dst[i] = '\0';
	return i;
}

/**
 * Write a 32-bit value as big-endian into buf (4 bytes).
 */
void ss_write_be32(uint8_t *buf, uint32_t v) {
	buf[0] = (uint8_t)(v >> 24);
	buf[1] = (uint8_t)(v >> 16);
	buf[2] = (uint8_t)(v >> 8);
	buf[3] = (uint8_t)(v);
}

/**
 * Write a 16-bit value as big-endian into buf (2 bytes).
 */
void ss_write_be16(uint8_t *buf, uint16_t v) {
	buf[0] = (uint8_t)(v >> 8);
	buf[1] = (uint8_t)(v);
}

/**
 * Write a 32-bit value as little-endian into buf (4 bytes).
 */
void ss_write_le32(uint8_t *buf, uint32_t v) {
	buf[0] = (uint8_t)(v);
	buf[1] = (uint8_t)(v >> 8);
	buf[2] = (uint8_t)(v >> 16);
	buf[3] = (uint8_t)(v >> 24);
}

/**
 * Write a 16-bit value as little-endian into buf (2 bytes).
 */
void ss_write_le16(uint8_t *buf, uint16_t v) {
	buf[0] = (uint8_t)(v);
	buf[1] = (uint8_t)(v >> 8);
}
